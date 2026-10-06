# code-review - every issue

The full list behind docs/code-review-findings.md: 483 distinct issues, merged from 915 verified findings (919 raised, 4 refuted). Each entry is one issue that one fix would close. The C-numbers are stable ids the findings report and the plan refer to. Within a section, entries run from high to low severity, bugs first.

Legend: severity high / medium / low; kind bug (wrong behaviour today), rule (breaks a stated project rule), structure (architecture), cleanup (duplication, dead code, stale text).

## Contents

1. [The one hit pipeline](#1-the-one-hit-pipeline) - 52 issues (9 high, 25 medium, 18 low)
2. [AI behaviours as classes](#2-ai-behaviours-as-classes) - 23 issues (4 high, 11 medium, 8 low)
3. [Dialogs and inspectors](#3-dialogs-and-inspectors) - 43 issues (3 high, 20 medium, 20 low)
4. [Structure as it has grown](#4-structure-as-it-has-grown) - 36 issues (0 high, 18 medium, 18 low)
5. [The graphics pipeline](#5-the-graphics-pipeline) - 51 issues (3 high, 26 medium, 22 low)
6. [Globals](#6-globals) - 4 issues (0 high, 2 medium, 2 low)
7. [Allocation is guarded](#7-allocation-is-guarded) - 29 issues (5 high, 11 medium, 13 low)
8. [Repetitive and redundant code](#8-repetitive-and-redundant-code) - 54 issues (0 high, 11 medium, 43 low)
9. [Data, saves and editor integrity](#9-data-saves-and-editor-integrity) - 78 issues (13 high, 39 medium, 26 low)
10. [UI library and HUD](#10-ui-library-and-hud) - 13 issues (2 high, 4 medium, 7 low)
11. [Core and platform](#11-core-and-platform) - 16 issues (1 high, 7 medium, 8 low)
12. [Harnesses and tools](#12-harnesses-and-tools) - 52 issues (1 high, 21 medium, 30 low)
13. [Documentation drift](#13-documentation-drift) - 32 issues (0 high, 8 medium, 24 low)

## 1. The one hit pipeline

### C0. Rolled blows can heal when the defender's soak is bigger than the blow
*high - bug - effort S*

ResolveAttack computes `(damage x margin x jitter - soak) x (1 - resist)` and never clamps `raw - soak` at zero. The wound floor only applies to a positive result. A negative result goes to Deal, which passes it to `target.Absorb`, so the defender is healed and the log says they "drink it in". Deal's unrolled branch does clamp, as docs/effects.md:424 requires ("soak can only blunt, never invert"). The two mitigation copies inside the pipeline now disagree. This is a divergence inside fx::Deal, not a bypass of it.

You can reach it from a new game. Brand carries plate_cuirass (armor 7, slash 0.5), and most skel_swarm blows (4 slash) come out near (4 - 7) x 0.5 = -1.5. The ledger books the heal as `pipeline`, so PipelineTest can't see it. It is probably a regression from narrowing the wound floor to `dmg > 0`.

- src/Game/Combat.cpp:110, :117
- src/Game/Effect/Effect.cpp:234, :254

Fix:
- Add one pure `Mitigate(raw, soak, resist)` to Defense.h that returns `max(0, raw - soak) x (1 - resist)`, and call it from both branches.
- Keep the wound floor on the rolled path only, because unrolled Ticks are per-frame bites.
- Add RollTest checks that soak > raw never goes negative unless resist > 1.

### C1. A monster's burst (Hagalaz) bolt never explodes when it hits the party
*high - bug - effort S*

skel_magus casts firebolt_burst, and MonsterBolt puts a blast on that bolt. ResolveSpellHit and ResolveThrowHit both detonate an area carrier that connects. ResolveMonsterProjectileHit has no such branch:
- It deals one rolled Bolt (full firebolt damage plus burn) to one member and returns true.
- The engine then retires the bolt without calling Expire, so the blast is lost. A Wind Ward deflection loses it too.

The magus aims at its threat target's lane, so its bolt nearly always connects. In practice it plays as a plain firebolt caster and bursts only when its lane is empty. No authored level places a magus yet, but the generator, Populate and the editor can. SpellTest only checks the catalog id, which is why nobody noticed.

- src/Game/DungeonWorld_Combat.cpp:2259 (missing branch), :2178 (the party side's branch)
- src/Game/Spell/ModifiedSpell.cpp:125
- src/Game/Projectiles.cpp:294

Fix:
- After the `n == 0` lane check, Detonate and return. Ideally use one helper that every resolver calls after its own lane test. Moving the check into the dispatcher doesn't work for throws, which use `throwBlastType` and must drop the item.
- Decide whether a Wind Ward should deflect a burst.
- Add an eval that expects the `blasts=` tally to move.
- Re-price the magus in ThreatProfile (src/Game/DungeonWorld_Load.cpp:588).

The blast's lost potency is covered under C2.

### C2. Monster attacks are built three ways, and shots and blasts drop what melee applies
*high - bug - effort M*

Monster melee builds strength, the offense stance, powers and crit-pierce into its attack. The plain shot uses only `{kind->damage, kind->accuracy, kind->damageType}`. A caster's MonsterBolt fires at the spell's power with raw accuracy. Where they have drifted apart:

- **Stance:** MonsterTarget::Evasion still adds the held-back `(1 - offense) x accuracy` to guard. So low-offense casters (skel_mage 0.45) get the extra guard and also shoot at full accuracy. That breaks the "one number, both sides" rule in docs/damage-system.md. Party casts do pay the stance.
- **Blast potency:** ActiveBlast stores no shooter, and ApplyBlastHit uses `AttackerPowers(attacker, 0)`. So a monster's blast ignores its `powers` (the magus has fire 0.55).
- **Strength:** shots ignore it. Only the eval `spawn` command sets strength, so this skews harness ladders (smallparty.eval:68), not play.
- **Smaller drift:** the volley gap is a hard-coded 0.2 s instead of spells.cat's gap/jitter. Crit-pierce is latent, since no monster authors it.

Where: src/Game/DungeonWorld_Combat.cpp:1481 (melee), :1783 (shot), :1145 (guard), :2720 (blast)

Fix:
- Write one `MonsterAttackProfile` for melee, the plain shot and the caster bolt. Leave Potent to the impact, or powers count twice.
- Carry the shooter into ActiveBlast.
- Your call: putting the stance on shots cuts caster accuracy from ~75-85 to ~34-47, which moves threat, pools and evals. If shots stay exempt, make the guard bonus melee-only.

The "no stance" comment at DungeonWorld_Load.cpp:581 describes the code; it doesn't record a decision. Commit 523cf5f9 added the stance to melee and guard only, which looks like an oversight.

### C10. A severe fumble's drop loses a torch's charge and allocates in the swing frame
*high - bug - effort S*

PartyFumble's Drop/Fling case copies the id (`const std::string id = held.typeId`), clears the hand with `held = ItemSlot{}`, then calls `DropItemInCell(id, cx, cz)` without passing the charge. Two problems follow:

- **Charge:** the default of -1 means "untouched", and TickFloorTorches refills such a torch to its full burn_time. A lit torch is a club and falls back on the default severe table (`drop`). So a nearly spent torch fumbled from Sera's hand lands full again. Every other drop site passes the charge.
- **Allocation:** both lines construct a std::string in an armed swing frame, and the debug CRT allocates for that at any length. DropAtPartyFeet already avoids exactly this.

Severe fumbles are about 1% of swings, and up to ~10% for an over-exerted, untrained swing. No AllocTest mode makes the party swing (-Melee is a monster swinging at the party), which is how this slipped through.

- src/Game/DungeonWorld_Combat.cpp:582, :596-597
- src/Game/DungeonWorld_Light.cpp:341

Fix:
- Take `ItemKindFor(held.typeId)` and `held.charge` first.
- Call `DropItemInCell(held.typeId, cx, cz, charge)`, then `held.Clear()`, and write the log line from `kind.nameKey`.
- Separately, add an AllocTest mode where the party swings, with a forced-fumble knob.

### C43. A burst bolt that stops in a shut door or a one-thick wall explodes on both sides of it
*high - bug - effort S*

A projectile moves first and is tested afterwards (src/Game/Projectiles.cpp:286). When it expires against a wall, its position is inside the rock or shut-door cell, and the blast detonates there (src/Game/DungeonWorld_Combat.cpp:2872). Propagation treats that solid centre as a phantom that emits in all four directions (src/Game/Blast.cpp:79). The doorway's rock flanks then deflect onto both open squares, so the near side and the far side each take 3 arrivals at distance 1.

In play, any Hagalaz bolt or skel_magus burst that misses and breaks on a shut door, locked ones included, fills the room beyond and strikes the panel from both faces. A one-cell wall leaks the same way. Detonate's own comment says this cannot happen. The RollTest case opens cells on one side only, so it cannot see the leak. The damage still goes through fx::Deal, just to the wrong squares.

Fix: detonate on the last open square along the flight. LandThrown already backs off to that square (src/Game/DungeonWorld_Throw.cpp:270), so share that as one helper. Do not change the phantom: the burst round the caster (spareCentre) depends on it. Add a RollTest case with open cells on both sides. Rerun blast-geometry.eval and pipeline.eval's door section, because the wall-face square drops from 4 arrivals to 1.

### C3. Attacker potency and the enchantment rider are applied by hand at each call site, and the copies already disagree
*high - rule - effort M*

fx::Deal owns only the defender's stages. Callers apply attacker potency themselves at 8 hit sites. src/Game/Balance.h:419 says no source "can quietly skip it", but the copies already differ:

- **Thrown enchanted weapon:** potency is applied in the physical type at launch (src/Game/DungeonWorld_Throw.cpp:83). The fire rider is then `atk.damage * elementBonus` with no fire potency (Throw.cpp:179, and the door copy at 238). The swing does it right (src/Game/DungeonWorld_Combat.cpp:1996). A flamebrand's fire is x1.3 swung but x1.0 thrown.
- **No on_crit on throws:** the payload packs only `on_hit` (src/Game/DungeonWorld_Load.cpp:1366), so a thrown serrated_blade never applies its crit bleed.
- **Monster blasts lose their powers:** ActiveBlast stores no shooter, so `AttackerPowers(attacker, 0)` returns nothing (Combat.cpp:2720). skel_magus loses its fire 0.55.
- **Fumbles (latent):** party fumble self-hits get no potency; monster ones do.

Related bug: ResolveMonsterProjectileHit has no blast branch, so a magus burst that hits the party never bursts.

Fix, in two steps:
1. Add one enchant-burst helper for the swing, the throw and the throw at a door, built from the pre-potency damage and made potent in its own element. Carry `on_crit` in the payload, and pass the shooter through Detonate and ActiveBlast.
2. Put a potency table on DamageEvent, taken at launch, and apply it in Deal. ThreatProfile should call the same function.

Decide and write down whether light scorch/crackle, Fire Shield and DoT ticks take potency. The verifiers treat those as design questions, not bugs.

Against a documented decision: docs/damage-system.md:768 applies potency "wherever an attack's DamageEvent is built". The case for moving it is that the copies no longer follow that rule. The checked invariant still holds: health moves only through fx::Deal.

### C4. Everything after fx::Deal is hand-written at each hit site, and the copies have drifted
*high - rule - effort L*

fx::Deal owns the roll, soak, resist, wards and the health write. Everything after it is re-coded at about 12 hit sites, and those copies have drifted. That covers potency, the enchantment burst, on_hit/on_crit procs, fumble consequences, React, flinch, narration and training. The drift found:

- **Throw burst potency:** a thrown enchanted weapon's burst gets its physical type's potency, not its element's (src/Game/DungeonWorld_Throw.cpp:83, :179). The swing does it right at src/Game/DungeonWorld_Combat.cpp:1996.
- **Throws lose on_crit:** the throw payload packs only onHit (src/Game/DungeonWorld_Load.cpp:1366).
- **Flinch:** extra `hitReq = true` writes (DungeonWorld_Combat.cpp:2217, :2746, DungeonWorld_Throw.cpp:203) make an immune monster flinch from a bolt, blast or throw, but not from a swing.
- **Fumble self-hits** skip React and TrainDefense (DungeonWorld_Combat.cpp:613-636).

The effect on play is small today: only flamebrand carries powers and only serrated_blade has on_crit, and neither is placed in a level.

Fix: make the small fixes above first. Then add one two-phase DungeonWorld helper:
- **Resolve:** potency from raw damage, Deal, burst.
- The caller narrates.
- **Conclude:** procs, fumble hook, React, flinch.

Against a documented decision: CLAUDE.md leaves React to the caller so the reprisal line follows the blow. Keep that order, but move the other steps into the helper.

### C6. "Water puts out fire" and other element rules sit in spell and world code, not in fx
*high - rule - effort L*

Fire and water as damage never touch fires or burns. A waterbolt, water blast or frost hit leaves a burning mummy alight. A firebolt or fire blast never lights a sconce. Lighting and dousing exist only as one-off code:

- `setFireAhead` in Flame and Splash.
- `KindleNear`.
- `QuenchParty`, which only works for the party, only under a Tidelight, and erases effects by checking `Plume()`, a flag documented as presentation only. So it would also erase a frost chill. It also lets a burn land and erases it later, so the log says "catches fire" and then "quenches".

The brazier threshold is written three times: Flame 14, Splash 12, and `kindle_brazier_power` 14 with a hard-coded `14.0f` fallback.

The ignite rule in `MonsterTarget::Wound` is inside `fx::Deal`, so no health bypasses the pipeline. But it hand-rolls Apply plus the log line. Two consequences:
- The firebolt's own `on_hit` burn refresh cuts the 6 s ignite to 4 s.
- A magical torch ignites in plain orange, because `DamageEvent` carries no tint.

Where:
- src/Game/DungeonWorld_SpellLight.cpp:223
- src/Game/DungeonWorld_Combat.cpp:1214
- src/Game/Spell/Flame.cpp:43
- src/Game/Spell/Splash.cpp:34

Fix: work in stages.
- **Burn reacts to water.** Override `BurnEffect::OnStruck` so a water hit douses a fire-typed burn. This uses the existing hook, keyed on `DamageTypeOf`, not `Plume()`.
- **Tidelight refuses burns.** Make it a refusal in `fx::Apply` instead of a per-frame erase.
- **Ignite through ApplyProcs.** Route the ignite through `ApplyProcs`.
- **Fixtures own their thresholds.** Add fixtures.cat `ignite_power` / `douse_power`.
- **Ask Michael** whether bolts and blasts should light or douse fixtures. Use a zero-damage contact event for that, not a Burst, which would wound and provoke.

Against a documented decision: tier-1 HandSpells reaching the world through CastServices, and "the one seam is `MonsterTarget::Wound`".

### C49. The first blast of a session allocates in a guarded frame
*high - rule - effort S*

`m_activeBlasts` is never reserved, so the first detonation in a process grows it with `push_back` inside a guarded play frame. Each element is about 6 KB (256 Hits). Every new peak of overlapping blasts grows it again, for example an 8 s poison gas plus a burst. The first blast can be:
- a Hagalaz ward burst during the cast itself
- a bolt impact or expiry
- a thrown flask

It is the only allocation on that path. Verifiers called the cost bounded, a few allocations per process.

Where:
- src/Game/DungeonWorld.h:3440
- src/Game/DungeonWorld_Combat.cpp:2652
- src/Game/DungeonWorld_Ahead.cpp:177
- tools/AllocTest.ps1:1030

Fix: reserve about 8 in the constructor, as ProjectileSystem does (src/Game/Projectiles.h:230). Never evict a live blast, or a lingering gas would silently stop biting. The same task found two more first-use allocations: TrainDefense's static vectors and PartyFumble's `kNone` (src/Game/DungeonWorld_Combat.cpp:136, :641).

Against a documented decision: AllocTest -Impact counts "the first detonation" as process warm-up (tools/AllocTest.ps1:1061). That conflicts with CLAUDE.md's "a bug, full stop" and with the AudioEngine reserve precedent.

### C5. An enchanted weapon's burst wounds the monster it just killed
*medium - bug - effort S*

After a landed blow, PartyAttack and ResolveThrowHit deal the weapon's elemental Burst without checking whether the blow killed. MonsterTarget::Wound has no dead guard (Absorb and BreakableTarget::Wound both have one), so the corpse is wounded again. As a result:
- the harness `slain` tally counts the kill twice;
- the burst's damage is added to the "hits for N" line;
- threat and a provoke land on a dead monster, which can print a spurious "locks onto" line.

Every lit torch is enchanted, and Sera starts holding one. Nothing else in play changes: hp clamps to 0 and the ledger balances.

- src/Game/DungeonWorld_Combat.cpp:1991 and src/Game/DungeonWorld_Throw.cpp:177 (the bursts)
- src/Game/DungeonWorld_Combat.cpp:1190 (Wound) and :1175 (Absorb's guard)

Fix: first skip the burst when `ev.slew`, as StrikeDoorWithThrow already does (Throw.cpp:237). Then add `if (!m_monster.Alive()) return;` at the top of Wound as a backstop.

### C9. A Sowilo light running out prints the Sight spell's fade line
*medium - bug - effort S*

The party's effect-expiry handler chooses its message by effect category, and `Category::Marker` prints `log.sight_fades`. LightEffect is a Marker, so every carried Firelight, Tidelight or Skylight (60 s) ends with "The stone closes to Maren's sight." Stonelight is not affected, because it places a stone rather than an effect on the caster.

- src/Game/DungeonWorld.cpp:1073 (the category switch)
- src/Game/Effect/LightEffect.cpp:13

Fix: let each effect kind name its own fade line, either with an effects.cat `fade_party` / `fade_monster` or with an OnExpire hook. Then drop the category switch.

### C11. A bolt the Wind Ward turns aside still trains `avoid`
*medium - bug - effort S*

ResolveMonsterProjectileHit calls `TrainDefense(target, ev)` one line before `if (ev.deflected) return true;`. A deflected Bolt keeps `rolled = true` and `hit = false`, so for an unarmored member LessonFrom returns Avoid. That member gets avoid XP and DEX creep from a bolt the ward stopped. This breaks docs/damage-system.md ("never evaded and never turned"), and the call site's own comment says "a dodged bolt".

- src/Game/DungeonWorld_Combat.cpp:2312-2313
- src/Game/Defense.cpp:141

Fix: move TrainDefense below the deflected return, or have TrainDefense return early on `ev.deflected`. Keep Defense.h pure, so don't pass it the whole event.

A verifier narrowed this:
- An attacker's fumble training avoid follows the documented rule (a fumble is a miss), so it is not a bug.
- A small real leak remains: the armour lesson reads total Soak(), so a pierce crit that skipped soak still trains armour.

### C17. The light flare hits monsters it never reached and lights fires through rock
*medium - bug - effort M*

The Hagalaz flare is meant to reach "every monster within 3 walking steps". Inside LightFlare, three different rules decide what that means:
- **Dazzle** uses the light budget's reach map. That map walks floor cells only, so it passes through shut, locked and flag-sealed doors.
- **Scorch / shock** hits every dazzled monster on the level, including ones still dazzled (2-8 s) by an earlier flare cast somewhere else.
- **Kindle** uses Manhattan distance, so it lights a sconce in a parallel corridor through rock.

Damage is small (about 1.5-3.4 per hit) and still goes through fx::Deal.

- src/Game/DungeonWorld_SpellLight.cpp:142, :154-164, :261

Fix: give StoneReachCells (:365) a step count and Detonate's open-square test. Collect the monsters this flare dazzles in a fixed array, and scorch or shock only those. Kindle only fires inside the reached set; a brazier's own cell is unwalkable, so test its neighbours. The same door-blindness affects CrackleNearest and Stonelight's tracks.

### C18. A gust's repel weakens a shot's direct damage but not its blast or procs
*medium - bug - effort S*

Repel compares cast power against `it.atk.damage` and changes only that field. A burst bolt's `payload.blast` and its on-hit procs keep full strength, and a blast carrier is resolved only by its blast. So a "weakened" magus burst bolt still explodes at full force, and a firebolt's burn still lands in full. In the rare case where power exactly equals the shot's strength, the shot gets range 0 and expires next update in or beside the party's square, detonating at full force.

- src/Game/Projectiles.h:351-362
- src/Game/DungeonWorld_Combat.cpp:2872

Fix: scale blast damage and proc magnitudes by the share that survives, as `throw_scale` does at load. Remove a spent shot without an expiry.

A verifier narrowed this. The power-versus-strength arithmetic is Michael's rule (spells.md), and the shot still resolves through fx::Deal. Separately, spells.md says thrown items are repelled too, but the code skips cargo.

### C19. A modified bolt spell ignores its own spells.cat on_hit
*medium - bug - effort M*

The volley and burst bolts of a ModifiedSpell are built by the base spell's PartyBolt or MonsterBolt. BoltSpell::MakeBolt fills `bolt.payload` from the base spell's payload, and LendLook copies only the light and trail. So `[firebolt_volley] on_hit = burn 1 4` is dead data, and each volley bolt burns at firebolt's `burn 2 4`. The ward-burst path does use its own payload, and the harness `blast` command measures the modified spell's payload rather than what a real cast delivers.

The same root is masked elsewhere today:
- Only BoltSpell reads `push`.
- The constructor reads the base spell's power and mana before overrides are applied.
- 15 `effect =` and 15 `element =` lines in spells.cat are read by nothing.

- src/Game/Spell/ModifiedSpell.cpp:92, :122-126
- src/Game/Spell/BoltSpell.cpp:41

Fix: apply the modified spell's authored procs to the built bolt, forward `push`, derive the defaults after SpellBook::Build, and warn on keys nothing reads.

### C34. A missed or fully turned attack neither wakes a monster nor breaks rest
*medium - bug - effort S*

ProvokeMonster is called only from MonsterTarget::Wound and Absorb, and BreakRest("attacked") only from WoundMember. fx::Deal returns early on a deflect or a miss, and calls neither Wound nor Absorb when the damage is exactly 0. As a result:
- A missed shot at an `asleep` monster or a dormant lurker beyond its 2-square trigger leaves it asleep.
- A fire bolt on a fire-immune creature is never noticed.

The provoke half is the real defect, because it affects stealth and ambushes. The rest half matters less, since the next blow that lands breaks the rest.

- src/Game/Effect/Effect.cpp:237, :253
- src/Game/DungeonWorld_Combat.cpp:56, :1180, :1202

Fix: after Deal, call an `ITarget::Noticed(ev)` hook for every non-Tick event that reached a target. A monster provokes and credits threat; the party breaks rest.

Against a documented decision: CLAUDE.md ends rest on `attacked`, defined as "a blow", with WoundMember's quiet flag marking the line. This change would make a swing that misses or is turned aside count too.

### C36. Stat-up log lines print raw keys such as "stat.intelligence"
*medium - bug - effort S*

GrantStatPoint builds its message key as `stat.<id as passed>`. The lang files define only stat.strength, dexterity, stamina and health, and have no stat.intelligence, willpower or vitality. So every INT or WIL point a caster earns prints "Maren's stat.intelligence rises to 11". The throw's abbreviated list `{"str", "dex"}` prints `stat.str` / `stat.dex`.

- src/Game/DungeonWorld_Combat.cpp:437
- src/Game/DungeonWorld_Throw.cpp:46
- assets/lang/en.lang:476, :693

Fix: name the stat through `attr.<kStats[i].id>`; all five lang files define all five, and the index is already resolved. Move the throw's stats into Balance as full ids, and drop the dead `stat.stamina` / `stat.health` keys.

Related and unverified: TrainDefense's function-local static lists (Combat.cpp:136) are built on the first rolled blow, which may allocate inside a guarded frame.

### C40. Thrown bombs never train `throwing`
*medium - bug - effort S*

When a fire or poison flask (any size) hits, ResolveThrowHit's blast branch calls Detonate and returns before reaching the throwing XP award. LandThrown's shatter path trains nothing either. Stamina and exertion are still charged at the throw. So a rock that lands on a monster trains `throwing`, but a flask that lands on the same monster does not. The plan says throwing XP comes "on a landed hit like a melee blow".

- src/Game/DungeonWorld_Throw.cpp:144-147 (early return), :192-198 (award), :290 (shatter)

Fix: award the XP on contact, before the Detonate branch, and decide explicitly whether a wall shatter counts.

### C44. A bolt that breaks on a wall delivers its on-hit to nobody
*medium - bug - effort S*

For a Wall expiry, `expiry.pos` is already inside the rock or shut-door cell. ResolveProjectileExpiry looks for victims in that cell, where nobody can stand. So the documented case "broke on the wall behind you still catches you" never happens; only Range expiries in open air deliver. The doc's own check tested only a Range expiry. LandThrown backs off to the last open square, so bolts and throws disagree about where a flight ends.

The reach is narrow: it only affects a body the lane missed in the last open square, and today only the burn from firebolt and firebolt_volley is lost.

- src/Game/Projectiles.cpp:286-291
- src/Game/DungeonWorld_Combat.cpp:2864-2918
- src/Game/DungeonWorld_Throw.cpp:273-286

Fix: move LandThrown's back-off into a shared `LastOpenCell` helper and use it for the Wall proc pass, after StrikeDoorWithBolt. Add an eval for this case.

### C45. Solid props stop bodies but not bolts, blasts or sight
*medium - bug - effort M*

Movement respects `deco.Blocks()` in three places: the party's isOccupied, the AI's blocked grid and FreeSlotInCell. The missile test, Detonate's passability check and line of sight know only rock, bores and doors. decorations.cat marks pillar, statue, boulder, barrel, crate and portcullis `solid = 1`. Pillars are placed decorations now, so in play:
- A party sheltering behind a pillar is hit by bolts that pass through the stone.
- A fireball rolls through a boulder.
- A breakable barrel blocks every body but neither stops nor takes a firebolt.

- src/Game/DungeonWorld.cpp:267-277 (missiles)
- src/Game/DungeonWorld_Combat.cpp:2624-2629 (blasts), :1797-1815 (line of sight)

Fix: add a per-kind `stops_missiles` flag that defaults to `solid` (a portcullis opts out), and use it in all three tests. Also noted: line of sight ignores closed doors, and the host and AI line-of-sight checks disagree.

### C47. Saving with a flask in flight detonates it, possibly on the party
*medium - bug - effort M*

SaveGame lands any thrown item before it captures state, through LandThrownItems, LandCargo and Expire. For a `throw_breaks` item, "landing" means LandThrown resolves the payload. A fire or poison flask therefore detonates in the live world at its current square, which may be the party's square or the one next to it, and a blast catches the party. Play then resumes after the pause-menu save, and the saved state already includes the damage. A rock is simply dropped short of its target.

- src/Game/Game.cpp:1166-1168
- src/Game/Projectiles.cpp:145-152
- src/Game/DungeonWorld_Throw.cpp:290-296

Fix: write cargo in flight into the save data as a drop at its landing square, without touching the live flight.

Against a documented decision: CLAUDE.md THROWING says a save lands thrown items first (LandCargo). That is right for a rock, but for a shattering item landing means detonating, and a save should never do that.

### C48. At 60x rest, projectiles skip over the party and through thin walls
*medium - bug - effort M*

ProjectileSystem::Update moves a flight one whole step per frame and only tests where it ends up (src/Game/Projectiles.cpp:280-296). Rest multiplies world dt by 60 and does not sub-step (src/Game/Game.cpp:1920, src/Game/Balance.h:225). At 60 fps a 6-8 m/s shot therefore moves 2.4-3.2 squares per frame.

A hit on the party needs that end point to be in the party's square (src/Game/DungeonWorld_Combat.cpp:2263), and isBlocked checks only one cell (src/Game/DungeonWorld.cpp:267-277). So a shot at a resting party can:
- jump the party's square, so rest never ends as "attacked";
- skip monsters in its lane;
- pass through a wall one square thick.

Fix: split each step into pieces of at most half a square, or walk every cell the segment crosses. The harness `step` already uses fixed sub-steps (src/Game/Game.h:117-127), so only rest and `timescale` are exposed.

Against a documented decision: REST multiplies time "at ONE place". That only works if everything that reads dt copes with large steps, and the projectile code does not.

### C7. Spell power and the cast-fail roll are hard-coded instead of balance knobs
*medium - rule - effort S*

MagicSystem::Cast has six balance numbers written into the code:
- 0.10 power per school level (src/Game/Magic.cpp:80). Weapons use the `skill_damage` knob (0.08) for the same job.
- The fail roll's 0.35 / 0.10 / 0.01, its 0.9 clamp and its 0.95 cap (src/Game/Magic.cpp:64-69).

Balance has only one spell knob, `spell_stat`, so the Balance dialog cannot reach any of these.

Fix: add kBalanceFields rows for the six numbers, read them through the `m_balance` that Cast already holds, and keep the formula as it is.

Verifiers narrowed this. No shared attacker-side builder is needed:
- Stance, exertion and haymaker maths already go through shared defense:: helpers.
- The stat terms differ on purpose.
- Casts ignoring exhaustion is by design ("Casting stays mana's business", docs/combat.md:360).

Against a documented decision: CLAUDE.md MAGIC and docs/skills.md:63-71 write these numbers down as fixed, while docs/combat.md:216-223 says every constant is a knob. The docs disagree with each other. Two lines are stale either way: combat.md's "ONE shape" line and skills.md:72's line on bolt accuracy.

### C8. What light, sight and dazzle do is coded in DungeonWorld, and the effect modifier hooks are unused
*medium - structure - effort M*

The light, sight and dazzle effects are markers only. What they actually do is world code that looks the effect up by id or school, for example LightFlare's `switch (school)` (src/Game/DungeonWorld_SpellLight.cpp:150-183). Their damage still goes through fx::Deal. Concrete problems:
- Expiry wording is picked by effect category in world code (src/Game/DungeonWorld.cpp:1069-1081). LightEffect is a Marker, so when a member's Firelight ends the log says "The stone closes to X's sight."
- Sight ignores its power. Depth and radius are fixed per school (src/Game/DungeonWorld.cpp:894-895), and Farsight.h's note about magnitude is stale.
- StatBonus and SpeedScale are never called (src/Game/Effect/Effect.h:339-340). Dazzle, the first modifier-style effect, bypasses them with an id check (DungeonWorld_SpellLight.cpp:113).
- The scorch and crackle formulas each appear twice (:153/:299, :161/:320), and the dazzle duration is an inline clamp (:135).

Fix: give each effect kind its own expiry wording, and make Sight read its magnitude. Move the numbers into effects.cat, then either route dazzle through the hook or delete the hook. The per-type DoT sum is correct as it is.

Against a documented decision: CLAUDE.md THE LIGHT FORM has the world read these effects every frame, and docs/effects.md decision 4 keeps the unused hooks on purpose.

### C15. Bolts and throws only hit doors, though the breakable code says they hit everything
*medium - structure - effort M*

The comment on ForEachBreakableAt says every damage source goes through it, "blasts, bolts and anything later" (src/Game/DungeonWorld_Combat.cpp:2392-2396). In fact only blasts, SmashAt and ApplyEffectToBreakables call it. A stopped bolt checks doors only (DungeonWorld_Combat.cpp:2828), and so does a throw (src/Game/DungeonWorld_Throw.cpp:218). As a result, a bolt or a throw never hits a breakable prop or fixture.

A sconce or brazier is also stored as two structs with two effect lists, aged by separate code: `Fire::effects` (DungeonWorld.h:3025) and `FixtureBreak`'s list.

Fix:
- Store one record per fixture.
- Pick the target of a stopped flight through ForEachBreakableAt.
- Correct the comments and docs/damage-system.md:674-677 (smash is an unrolled Impact now).

The blast's loop over the adjacent door is meant to be different. Swings at scenery are a documented future seam. Giving fires an element state (doused or lit by a hit) needs Michael's decision first.

### C20. Monsters cast spells through a separate bolt-only path that has drifted
*medium - structure - effort M*

Spell::Cast needs a Character, so monsters cast through MonsterBolt/MonsterVolley and the world code assembles the rest (src/Game/DungeonWorld_Combat.cpp:1761-1773). The two paths now differ:
- The monster volley uses a fixed 0.2 s gap with no jitter. The spell's own `gap` (0.18) and jitter are ignored (src/Game/Spell/ModifiedSpell.cpp:89-99).
- The threat estimate looks at one MonsterBolt only (src/Game/DungeonWorld_Load.cpp:585-591). A volley rates at 0.6 of one bolt and a burst's blast is left out, so skel_mage_adept ranks below skel_mage and skel_magus is underrated.
- ResolveMonsterProjectileHit has no blast branch. A magus burst bolt that hits a party member does plain bolt damage and only explodes if it misses and flies on.
- Bolts already queued keep launching after their caster dies.

Fix: one Cast(ctx) for any caster, carrying target side, power, accuracy and shooter. A smaller step is a MonsterCast plus a ThreatShot that the threat estimate reads. The bolt itself is already shared, through MakeBolt.

### C21. Shove and Gust's repel are worked out outside the effect pipeline
*medium - rule - effort M*

Airbolt's shove is a fixed step run after fx::Deal (src/Game/DungeonWorld_Combat.cpp:2228). It rides on a `push` field that only bolts have, so a weapon cannot shove, and the monster path ignores `push` entirely.

Gust's repel compares raw cast power with a shot's damage before potency, inside the projectile engine (src/Game/Projectiles.h:347). Damage type and resists play no part.

Fix: make shoving an effect proc (`on_hit = shove 1`), and give the air-against-projectile contest one owner next to fx.

Verifiers dropped the "two copies" claim. ShoveMonster is already the only shove code, reached from two places, and repel does not duplicate Windward's deflect, which uses charges.

### C23. ModifiedSpell chooses its behaviour with dynamic_cast on the spell it wraps
*medium - structure - effort M*

IdFor and the constructor use dynamic_cast to tell which form they wrap (src/Game/Spell/ModifiedSpell.cpp:26, :37-39). Cast then branches across six behaviours: two for light, two for bolts, two for wards (:71-117). AllSpells repeats the same three casts to decide which forms take a modifier (src/Game/Spell/AllSpells.cpp:77).

Adding a form, or allowing a modifier on Sight, means editing all of these. Each form's modified behaviour also lives away from the form itself.

Fix: add virtual hooks to the form base (TakesModifier, ModifierSuffix, CastModified), and have each form implement its two cases. ModifiedSpell keeps the knobs, the id and the forwarding, which still fits CLAUDE.md's "one ModifiedSpell class".

### C24. CastServices has grown into 17 world calls, most used by a single spell
*medium - structure - effort M*

CastServices has 17 std::function hooks, wired up by position (src/Game/Spell/Spell.h:64-131, src/Game/DungeonWorld.cpp:223-257).
- Only spawnBolt, spawnBoltAfter and applyEffect are general-purpose.
- Most of the rest serve one spell. For example, fillItem serves only Splash, and shoveAhead and repelAhead serve only Gust.
- Each new idea adds a field, a lambda and a DungeonWorld method.

The root cause is that applyEffect only accepts a Character&. Nothing can land an effect on a monster, a fire or a held item through the general channel.

Fix: let applyEffect take other targets, group the "ahead" calls behind one interface, and move the flare's per-school switch out of world code (see C8).

Verifiers narrowed this:
- blastAroundParty already goes through Detonate.
- The flare's damage already goes through fx::Deal.
- The hand-spell calls act on world objects, so they cannot become projectile or effect calls.

Against a documented decision: CLAUDE.md MAGIC says spells reach the world only through CastServices. That boundary is right; the problem is how it has grown.

### C28. The projectile structs repeat the same flight fields, filled in by position
*medium - cleanup - effort M*

Four structs each declare the same flight fields (pos, dir, atk, attacker, shooter, payload, cargo, cargoCharge): ProjectileSpec, the private Item, ProjectileImpact and ProjectileExpiry (src/Game/Projectiles.h:132, :181, :201, :392).
- Spawn copies 17 fields by hand (src/Game/Projectiles.cpp:24-44).
- Impacts, expiries and Live/Find are filled in by position (:158, :166, :248-249, :295-296).
- In Impact, `push` and `attacker` are neighbouring ints. If they were reordered the code would still compile and silently swap them.

ProjectileInfo has fewer fields, so the count is four structs, not five.

Fix: make Item hold a ProjectileSpec, have the hooks take a read-only view of it, and use designated initializers elsewhere. The header comment (Projectiles.h:5-7) still lists thrown items as future work.

### C29. A projectile's payload does not carry its own damage type, flavour or strike kind
*medium - structure - effort M*

These are supplied or patched by callers instead:
- **Damage type.** BlastSpec has none (src/Game/Projectiles.h:81-88), so five callers each pass Detonate one, and the throw path patches `burst.atk.type` (src/Game/DungeonWorld_Throw.cpp:294).
- **Flavour.** MakePayload returns none. Four callers add it themselves, and one already forgets: src/Game/Game_DevParty.cpp:636.
- **Strike kind.** It is decided by whether `cargo` is null (src/Game/DungeonWorld.cpp:282).

The same firebolt_burst payload also lands differently depending on what carries it. A bolt that hits a wall explodes in the wall square. A flask backs off to the last open square and explodes there (Throw.cpp:273-295).

Fix: put the damage type and flavour on the payload, filled once at load. Add an explicit strike descriptor to ProjectileSpec instead of testing `cargo`.

### C30. Projectile range and speed mix metres and squares
*medium - rule - effort M*

The units differ by source:
- **Spells: metres.** ProjectileSpec takes speed in m/s and range in metres (src/Game/Projectiles.h:135-136). spells.cat writes `range = 8`, which is 3.2 squares (assets/projects/dungeon-demo/catalog/spells.cat:51), and its header never names the unit.
- **Throws: both.** Range is in squares (src/Game/DungeonWorld_Throw.cpp:103) but speed is in m/s.
- **Monster shots: both.** Range is in squares, and speed is a hard-coded 6.0 m/s (src/Game/DungeonWorld_Combat.cpp:1779-1780).

If kUnit changes, throw reach scales and spell reach does not, which breaks the SCALE rule. The mix has already caused one harness mistake (tools/AllocTest.ps1:1010-1012).

Fix: write range and speed in squares and squares per second everywhere, convert once in Spawn, document the units, and make the monster shot speed a knob.

### C35. TrainDefense's static stat lists allocate on the first blow, inside a guarded frame
*medium - cleanup - effort S*

TrainDefense declares two stat lists, `kDexStat` and `kStrStat`, as static `std::vector<std::string>` inside the function (src/Game/DungeonWorld_Combat.cpp:136-137). They are built the first time a monster's rolled blow or dodged bolt reaches a member (:1493, :2312). That happens mid-play, in a frame the allocation guard watches.

Balance.cpp:357-360 and DungeonWorld_Throw.cpp:42-44 already fixed this same pattern. AllocTest -Melee's first-blow warm-up hides it (tools/AllocTest.ps1:70-72). PartyFumble's empty `kNone` vector (:641) also allocates in debug builds.

Fix now: move both lists to namespace scope next to Balance's, and replace kNone with an empty span.

A bigger follow-up (M): stat lists are heap strings matched by string compare on every swing and skill award. A set of stat indices parsed at load would remove that.

### C50. The "dim as it runs out" rule and its reach shrink are copied across three files
*medium - cleanup - effort S*

The dim constants `kDimShare` / `kDimFloor` (0.1, 0.35) are declared twice. `TorchBrightness` and `DimFor` do the same arithmetic, and the spell copy even calls itself "the torch's rule". The reach shrink `radius * kCellSize * (0.6 + 0.4 * brightness)` is written out in five places. To retune how a dying light fades you have to find every copy, and missing one makes torches, spell lights and stones fade differently.

- src/Game/DungeonWorld_Light.cpp:43, :214, :369, :387
- src/Game/DungeonWorld_SpellLight.cpp:34, :56, :106, :547
- src/Game/DungeonWorld_Flight.cpp:139 (already calls TorchBrightness; only the reach formula is copied)

Fix: keep one `DimFor` and write `TorchBrightness` through it. Give `PushLight` a `dim` share that applies both the brightness and the reach shrink.

### C12. Soak and resists count the hand slots, but the armor class does not
*low - bug - effort S*

`PartyTarget::Soak`, `::Resist` and DefenseFor's soak sum all walk every equipment slot, hands included. `WornArmorClass` skips the hands, and its own comment calls the Soak behaviour a bug. DefenseFor also adds up soak again by hand, even though its comment says it is "built by asking the live path".

This can't happen in play today: hands accept only holdable items, and no holdable item has armor or resists.

Fix:
- Use one worn-pieces helper with one hand rule (for example, a shield flag) for all four defensive sums.
- Have DefenseFor read `PartyTarget::Soak`.
- Leave PartyPowers alone: counting only the swinging hand is correct there.

Refs: src/Game/DungeonWorld_Combat.cpp:864, :907, :1061, :1068.

### C33. Party melee hits whichever monster comes first in the list for the square ahead
*low - bug - effort M*

PartyAttack takes the first live monster on the target square (src/Game/DungeonWorld_Combat.cpp:1864). It ignores the monster's slot and the attacker's lane and rank. When several monsters share a square, Brand on the front left can hit a back-corner monster while the one in front of him is untouched. This is rare today: every monster defaults to Large, and only skel_swarm (Medium, two in crypt2) shares a square.

Fix: add a `PickMeleeTarget` that prefers the front slots, then the attacker's lane.

This is a missing rule, not a broken one. docs/movement.md's "symmetrically" only covers rank against reach.

### C39. Punch or Kick with a key in hand uses the key's empty skill
*low - bug - effort S*

When a held item has no command of its own, the hand menu offers Punch and Kick (src/Game/GameUI_Items.cpp:357-386). PartyAttack still takes the skill from the held item (src/Game/DungeonWorld_Combat.cpp:1884), and for keys and misc items that skill is "" (src/Game/DungeonWorld_Load.cpp:1240). The punch then swings at skill level 0, and GrantSkillXp silently throws away its XP (:75). The parry follows the same rule (:1016).

Fix: Punch, Kick, or a held item with no skill should use `unarmed`.

Narrowed by the verifier: the starter casters don't hold runes they already know, so this only happens with keys, misc items or a duplicate tablet.

### C46. Only monster occupancy knows a Huge monster covers 2x2 squares
*low - bug - effort M*

`FootprintCells` is used only by BuildAISnapshot and FreeSlotInCell. Party movement, melee, `MonsterInLane`, blasts and `orthoDist` all check only the monster's anchor cell. The first `size = huge` monster would:
- let the party walk into three of its four squares
- take bolts and blasts on only one quarter of its body
- stall on its west and north attack sides

This can't happen yet because no monster is authored as Huge.

Fix: one footprint-aware "does monster M occupy cell C" test that every check uses. docs/movement.md:522 is out of date as well.

Refs: src/Game/DungeonWorld.cpp:145, :2136, :2309; src/Game/DungeonWorld_Combat.cpp:1865.

### C13. Door strikes switch off `rolled` by hand instead of using the Impact preset
*low - cleanup - effort S*

StrikeDoorWithBolt (src/Game/DungeonWorld_Combat.cpp:2837) and StrikeDoorWithThrow (src/Game/DungeonWorld_Throw.cpp:231) build a Bolt or Blow event and then set `rolled = false`. They are the only callers that change a preset's flags, against docs/effects.md. `DamageEvent::Impact` already does what they need (not rolled, but soaked and resisted), so both should call it. No new preset is needed, and merging the two functions is optional.

### C14. DamageEvent `source` and `slew` mean different things on each side
*low - cleanup - effort S*

- **`source`:** documented as the attacker (src/Game/Effect/Effect.h:180), but MonsterAttack passes the victim (src/Game/DungeonWorld_Combat.cpp:1490) so the fire shield's counter-burn can give threat to the member who was hit (src/Game/Effect/WardEffect.cpp:64).
- **`slew`:** means "downed" on the party side (:1084) but "dead" on the monster side (:1206).

So a monster with a fire shield (possible via dev `effect fireshield ahead`) would print "slain" for a member who is only unconscious.

Fix: separate the attacker and the threat-credit fields, report downed and killed separately, and have the ward call `NarrateFall`.

### C16. The fire relight steps and the bracket-torch rule are repeated
*low - cleanup - effort S*

- **Relight steps:** the flare reset followed by Ignite or Clear appears in three functions (src/Game/DungeonWorld_Fires.cpp:46, :74, :194), and the copies have already drifted.
- **Bracket torch:** the "which torch is in this bracket" rule appears three times (Fires.cpp:118, :179; src/Game/DungeonWorld_Combat.cpp:2589).
- **Leader lookup:** TakeTorchAt finds the leader by hand and skips LeaderMember's alive check (Fires.cpp:128).

Fix: a `SetLit` helper that acts only on a change, plus `SconceTorch()` and a non-const `LeaderMember()`.

Narrowed by the verifier: the three brazier thresholds are separate cast-power settings, one per spell, as documented, not duplication.

### C22. Ward, Sight and Light spells repeat the same "apply an effect for a while" shape
*low - cleanup - effort S*

Each of the three spell forms calls applyEffect, prints a "log.<x>_up" line, and reads `duration` in its own ApplyOverrides (src/Game/Spell/WardSpell.cpp:25, SightSpell.cpp:17, LightSpell.cpp:25). SightEffect and LightEffect also repeat a per-school name table (src/Game/Effect/SightEffect.cpp:18, LightEffect.cpp:19).

The copies are small, and each form takes different modifiers and hooks. A merged EffectSpell class isn't worth it; a shared helper and a shared duration override are enough.

### C25. Cast services and cast context are built by position
*low - cleanup - effort S*

The 17 cast services are filled in by position (src/Game/DungeonWorld.cpp:223). `lightItem` and `fillItem` have identical types (src/Game/Spell/Spell.h:98), so swapping their order would still compile and would silently swap what Flame and Splash do to a held item.

- CastContext is also built by position (src/Game/Magic.cpp:101), even though its own comment warns against it.
- Its `schoolLevel` field (Spell.h:141) is never read.
- Null checks on the services are inconsistent, and Stonelight's fallback branch can never run.

Fix: named (designated) initializers, one check at startup that every service is set, and delete `schoolLevel`.

### C26. The particle bursts for hand spells, blasts, scorch and flares are hard-coded numbers in C++
*low - cleanup - effort M*

Sixteen Puff/Splash calls are written as literal numbers, so tuning one needs a rebuild:
- 9 in src/Game/DungeonWorld_Ahead.cpp from :193
- 2 at src/Game/DungeonWorld_Combat.cpp:2703
- 5 in src/Game/DungeonWorld_SpellLight.cpp from :126

The per-school glow strengths are also hard-coded, and they cancel out the `hand_puff` intensity in lights.cat. The code that picks a free glow slot is copied (Ahead.cpp:243, SpellLight.cpp:122).

Fix: burst entries next to trails.cat, plus a shared slot-picking helper.

This doesn't break a documented decision: CLAUDE.md makes lights and trails data, not every particle burst.

### C27. Dead Spell getters, unread spells.cat fields and stale headers
*low - cleanup - effort S*

- **Dead getters:** `Spell::Procs`, `LightId` and `TrailId` have no callers (src/Game/Spell/Spell.h:221).
- **Unread fields:** nothing reads the `effect` / `element` lines on 15 spells.cat entries (assets/projects/dungeon-demo/catalog/spells.cat:60, plus the template copies).
- **Stale headers:** the banners in BoltSpell.h, Waterbolt.h and Spell.h:5 describe the old design.
- **`blast_persist`:** ReadBlastRules can turn it on but never off (src/Game/Spell/Spell.cpp:58). Nothing starts out persistent today, so this can't happen yet.

Fix: delete the dead getters and fields, refresh the headers, and let `blast_persist` turn off as well as on.

### C31. The member's lane side and the world-to-cell conversion are rewritten at each call site
*low - cleanup - effort S*

PartyMemberSubPos says it is the one home for which side of the lane a member stands on. The same formula, `(faced + (member % 2 == 0 ? 3 : 1)) % 4`, is still copied in three other places:

- CastSpell: src/Game/DungeonWorld_Combat.cpp:2059
- ThrowItem: src/Game/DungeonWorld_Throw.cpp:89
- the held-torch light: src/Game/DungeonWorld_Light.cpp:399

`floor(x / kCellSize)` also appears 9 times. Detonate's passability lambda (src/Game/DungeonWorld_Combat.cpp:2623) is commented "the SAME test that stops a bolt". It is not: it drops isBlocked's bored-wall exception (src/Game/DungeonWorld.cpp:270). No level places a bore yet.

Fix: add `LateralOf(member)` and `CellOf(Vec3)`. Then either make blast passability allow bores or correct the comment. A verifier checked LandThrown's walkable test and found it answers a different question (where an item can rest), so it is not a copy.

### C32. The projectile system is also the world's general particle pool
*low - structure - effort M*

About 17 calls that have nothing to do with flight go through `m_projectiles.Puff/Splash/Mote` (src/Game/Projectiles.h:312): hand-spell puffs (src/Game/DungeonWorld_Ahead.cpp:196), light flares and track motes (src/Game/DungeonWorld_SpellLight.cpp:126-509) and blast puffs (src/Game/DungeonWorld_Combat.cpp:2703). One visible side effect: Mote marks its particle as a trail, so the `trails` readout counts track motes.

A verifier found the other claimed symptoms harmless. Clearing the pool on a level change is right for these effects too, and the header needs ParticleBatch.h anyway.

An optional tidy-up: move the pool into a `SparkPool` owned by DungeonWorld and lent to the projectiles, keeping the documented fixed-size shared pool.

### C37. kMeleeUses is a second hand-kept copy of Balance's attack verbs
*low - cleanup - effort S*

kMeleeUses (src/Game/GameUI_Items.cpp:27) lists the attack verbs the UI sends to onHandAttack. The real list is the Balance constructor's table (src/Game/Balance.cpp:190), and the two have already drifted: the UI list has `melee` and Balance does not. An item with `command = melee` would quietly fall back to the neutral attack (src/Game/DungeonWorld_Combat.cpp:1891). No catalog uses `melee` today, so nothing misbehaves. en.lang still has `use.melee`.

Fix: replace the list with one query that asks `FindAttack(verb) != nullptr`, excluding `throw`. Drop `use.melee`.

Against a documented decision: CLAUDE.md COMBAT lists "GameUI kMeleeUses" as a required step when adding a verb. This fix removes that step.

### C38. The inverse of the skill-level curve is written out by hand in three places
*low - cleanup - effort S*

Character::LevelForXp is `floor(sqrt(xp))` (src/Game/Character.h:280). Its inverse, `level * level`, is written by hand in three places:

- the sheet's progress bar: src/Game/CharacterSheet_Lists.cpp:193
- `setskill`: src/Game/Game_DevParty.cpp:1459
- the party-creation boost: src/Game/PartyRules.h:41

The curve is a fixed formula, not a balance knob (docs/skills.md), so these only drift if someone changes the code.

Fix: put LevelForXp, XpForLevel and LevelProgress in a small pure header that both Character.h and PartyRules.h include. PartyRules has to stay free of Character so RollTest can link it. SpellbookPanel::Match repeats SpellBook::Match, but it is display-only and the risk is negligible.

### C41. Lighting a magical torch spends mana but trains no attunement
*low - cleanup - effort M*

KindleTorch takes mana directly (`c.mana -= cost`, src/Game/DungeonWorld_Light.cpp:265) and grants no attunement. That breaks the documented rule that attunement trains by mana spent (docs/health-and-healing.md:103). A cast also takes mana directly (src/Game/Magic.cpp:36) and trains only on success (src/Game/DungeonWorld_Combat.cpp:2106). Stamina, by contrast, has one seam, SpendStamina (:455), which trains on every spend.

Fix: add a `SpendMana` that takes the mana and trains attunement, and use it for the torch and for casts.

A failed cast teaching nothing is a settled rule (docs/skills.md:44), so it is not a defect. The cast path must keep its success-only award unless that rule is deliberately reopened.

### C42. Skill XP amounts are hard-coded at four sites while the other training rates are knobs
*low - cleanup - effort S*

The XP grants for weapons, throws, avoidance/armour and school casting are literals:

- `kXp = 1.0f` for avoidance and armour: src/Game/DungeonWorld_Combat.cpp:131
- 1.0 for a landed blow: src/Game/DungeonWorld_Combat.cpp:2010
- `Mana() * 0.25f` for a school: src/Game/DungeonWorld_Combat.cpp:2098
- 1.0 for a throw: src/Game/DungeonWorld_Throw.cpp:197

The neighbouring rates (conditioning_xp, attunement_xp, constitution_xp) are balance.cat knobs (src/Game/Balance.h:186). As a result, the balance pass cannot change how fast weapons, throws, avoidance or schools level without a rebuild.

Fix: add plain `blow_xp`, `throw_xp`, `avoid_xp` and `cast_xp_per_mana` rows to kBalanceFields. The finding also suggested feeding these through a "learn stage" in the damage pipeline, but no such stage exists, so that would mean building it first.

### C51. Light-profile ids are string literals spread across files, plus a hand-kept check list
*low - cleanup - effort S*

SpellLightId maps a school to its profile name (src/Game/DungeonWorld_SpellLight.cpp:63). Eight sites skip it and type `"spell_earth"`, `"spell_water"` or `"spell_air"` directly, for example src/Game/DungeonWorld_SpellLight.cpp:210 and src/Game/DungeonWorld_Render.cpp:702.

ReloadLightProfiles keeps its own copy of every profile name the code uses, to warn when lights.cat lacks one (src/Game/DungeonWorld_Light.cpp:78, :117). The bolt and trail names are typed again in DungeonWorld_Flight.cpp:37. If a new profile name is added in code but not to that list, the warning quietly stops covering it.

Fix: declare the profile names once as constants plus an array of them. Use the constants at the call sites and loop over the array in ReloadLightProfiles.

## 2. AI behaviours as classes

### C52. A new game or reload can start with monsters already hunting the party
*high - bug - effort S*

The AI workers keep thinking while the world is frozen, on the title screen after a wipe or in the pause menu. They re-read the last snapshot, in which the fighting monsters were aware, and bump the plan seq every tick (src/Game/MonsterAI.cpp:349). ResetForNewGame clears `aware` but keeps every runtimeId (src/Game/DungeonWorld_Save.cpp:64). When the new or loaded game is on the level already in memory, the monsters stay in place. On the first frame, ConsumeAIPlans takes the stale batch and sets `aware` again for every non-Idle plan (src/Game/DungeonWorld.cpp:2217).

In play: wipe on crypt1, then Continue (a same-level load) or Start New Game. Every monster from that fight starts the game hunting the party on range alone, sleepers included. The loaded save's aware=0 rows are overwritten. The eval harness cannot see this, because lockstep and ResetForEval avoid it.

Fix: in ResetForNewGame, give each monster a fresh `runtimeId = m_nextMonsterId++` before RebaseDamageLedger, and clear aiPath/aiCursor. Pre-marking m_lastPlanSeq is not enough, because a worker mid-tick can still publish a newer seq.

### C53. Sight is checked by two functions that disagree, and neither is blocked by a shut door
*high - bug - effort M*

Brain perception uses SnapshotView::HasLineOfSight (src/Game/MonsterAI.cpp:76). It reads the pathing grid, where braziers block sight and bores do not exist. The host's CellHasLineOfSight (src/Game/DungeonWorld_Combat.cpp:1805) sees over braziers and through bores, yet its header calls it a "mirror" of the brain's check (src/Game/DungeonWorld.h:3530). Neither function reads doors. A doorway is a Floor cell, and shut doors go only into `blocked` (src/Game/DungeonWorld.cpp:2130).

In play:
- Monsters notice the party through closed doors.
- Kiters fire into a door and can burn a breakable wooden door down from behind.
- The Air light shocks through doors.
- In crypt1, the brazier hides the party from a skeleton while the party shoots it.

Doors only appear in generated and editor-built levels so far. The bore case is latent.

Fix: write one sight predicate and one LoS walker for both sides, published as a per-axis grid built next to `blocked`. Make door opacity a doors.cat field (the portcullis is bars, so it should stay see-through), and decide braziers on purpose.

### C58. Formation can send a monster to a cell it cannot enter, and it never attacks
*high - bug - effort S*

AssignFormation offers every Floor cell beside the party as an attack side (src/Game/DungeonWorld.cpp:1961). Braziers, solid decorations and shut doors all sit on Floor cells:
- **Brazier:** FindPath finds no path to it (src/Game/MonsterAI.cpp:229), so the monster stands still.
- **Crate or shut door:** FreeSlotInCell refuses the last step (src/Game/DungeonWorld.cpp:2319), and the same plan repeats.

Shipped repro: crypt1 has a brazier at (9,4) (assets/projects/dungeon-demo/levels/crypt1.map:21) and a skeleton at (10,4). With the party at (8,4), the brazier hides the party. The party can bolt the skeleton awake, and it then stands still for as long as the party stays put. Pass 2 fills empty sides first, so in a group someone always draws the bad side. The shut-door case is weaker.

Fix: write one "a monster can stand here" predicate and use it in the side list, in FreeSlotInCell and in BuildAISnapshot. Also check that a Huge monster's footprint fits. Delete the uncalled DungeonWorld::CellFreeForMonster (src/Game/DungeonWorld.cpp:2343).

### C62. Monster pathfinding allocates on every search, and resting runs it in guarded frames
*high - rule - effort S*

Brain::FindPath builds a new `std::queue<int>` (a deque) on every search (src/Game/MonsterAI.cpp:199). Every engaged monster not yet at its post therefore allocates on each think. On the workers this goes unchecked, against three comments and ARCHITECTURE.md's "8-50 allocations a session" (measured with idle monsters).

Rest turns on lockstep (src/Game/DungeonWorld_Combat.cpp:219), which runs this search on the main thread (src/Game/DungeonWorld.cpp:2201) in frames SteadyStateFrame arms (src/Game/Game.cpp:1747). The worst case is ordinary play: resting behind a shut door while an aware monster is in aggro range. Its search fails and allocates on every rest frame.

Fix:
- Make the queue a Brain member, a `std::vector<int>` reserved to W*H and walked by a head index (the RefreshReach pattern, src/Game/DungeonWorld_LightBudget.cpp:77).
- Pre-size the inline Brain at level load.
- Add AllocTest -Rest. It must wound the party first, or the rest ends at once.

Judging each worker tick is a separate, lower-priority item that ARCHITECTURE.md lists as not done.

### C54. A hit briefly forces kiters and fleeing monsters into melee mode
*medium - bug - effort S*

ProvokeMonster sets Engage on every blow (src/Game/DungeonWorld.cpp:2239), overriding the brain's Kite or Flee. Those plans carry no path, so a struck archer or fleer stands still until its next plan arrives (251-499 ms). If it is standing at its post it swings in melee instead (src/Game/DungeonWorld.cpp:1368). This is a short stutter per hit, not a lasting state.

Fix: put the mode choice in one pure helper that Think and ProvokeMonster both call. Do not run FindPath inline, because it allocates. Whether a ranged monster may ever melee is unsettled: Threat.h assumes it can, and src/Game/DungeonWorld_Combat.cpp:1394 says it cannot.

### C57. Kiters and fleers take melee attack sides, leaving brutes with nowhere to stand
*medium - bug - effort S*

AssignFormation gives a side to every aware monster, whatever its intent (src/Game/DungeonWorld.cpp:1969). A caster or archer takes one of the party's four sides and never walks to it. When sides are scarce (a corridor or a dead end), a brute that finds them all taken stays on its own cell (src/Game/DungeonWorld.cpp:2014) and never closes in.

Fix: only monsters whose intent is Engage take part in side assignment. Others keep their own cell as the target.

### C60. Leash return and patrol use greedy steps that can strand a monster for good
*medium - bug - effort M*

UpdateReturner and UpdatePatroller step greedily toward their goal (src/Game/DungeonWorld_Combat.cpp:1617, :1694), so a wall in the way stops them. Think returns Idle while a monster is beyond its leash (src/Game/MonsterAI.cpp:112). A sentry pulled round a corner therefore never returns and never re-engages.

This is latent: no level or catalog uses leash, patrol or sentry yet, and no eval suite covers them.

Fix: emit Return and Patrol goals for the worker's BFS, follow the path like Engage, and add an eval suite.

Against a documented decision: docs/ai.md P3b chose a host-side greedy patrol with no AI-layer change.

### C63. Starting lockstep does not wait for an AI tick that is still running
*medium - bug - effort S*

SetLockstep calls Pause and returns at once (src/Game/MonsterAI.cpp:284), but Pause only sets a flag (src/Core/ThreadManager.cpp:301). The comment saying Pause "takes effect at a tick boundary" is wrong. On the first rest frame, ComputeInline can touch the unlocked plan pool (src/Game/MonsterAI.cpp:324) while a worker is still scanning it, which is undefined behaviour.

A verifier narrowed this: only a worker caught in the middle of its pool scan can race, so it is extremely rare. Restart also clears `paused` (src/Core/ThreadManager.cpp:455).

Fix: wait for State::Paused before inlining, or give ComputeInline its own pool, and keep `paused` across a Restart.

### C64. Resting at 60x makes monsters slower and duller than awake ones
*medium - bug - effort M*

Rest scales one frame's dt (src/Game/Game.cpp:1920), so each frame gives the world about 1 s. TickLockstepAI runs at most one think per bucket per frame (src/Game/DungeonWorld.cpp:2189). A monster also takes at most one step per frame (src/Game/DungeonWorld.cpp:1371). The result per simulated second:
- Fast buckets think once instead of 4 or 2 times.
- A skeleton moves 1 square instead of about 2.2.

That breaks the promise at src/Game/DungeonWorld_Combat.cpp:207. `step` never applies the rest multiplier, so the harness misses it.

Fix: sub-step rest in fixed ticks, with a capped number per frame. Rest still multiplies time in one place, as CLAUDE.md requires.

### C65. A corpse in a doorway jams the door for good
*medium - bug - effort S*

`MonsterRuntimeIdAt` does not check `Alive()` (src/Game/DungeonWorld.cpp:1607-1611). Dead monsters are never removed from the list (src/Game/DungeonWorld_Combat.cpp:1204), and they stop being drawn once the death animation ends. So a monster killed in an open doorway leaves an invisible corpse, and from then on `ToggleDoor` refuses to close that door with "Something is blocking the doorway" (src/Game/DungeonWorld_Doors.cpp:236). The party's own `isOccupied` does check `Alive()` (src/Game/DungeonWorld.cpp:146), so the party walks through the square the door calls blocked. The corpse is saved too, so the jam survives a reload. The editor's AddDoor (DungeonWorld_Doors.cpp:190) and its door move (Move.cpp:259) use the same over-strict test.

Fix: use a live-only query in gameplay and keep the corpse-inclusive one for editor inspection.

### C69. Dead AI worker slots keep a job that points at the destroyed director
*medium - bug - effort S*

Each AsyncDirector worker job captures `this` (src/Game/MonsterAI.cpp:257-259). `~AsyncDirector` only calls `Stop` (src/Game/MonsterAI.cpp:270-271). Stop joins the thread and marks the slot Dead, but the slot keeps its stored job (src/Core/ThreadManager.cpp:288-297). The THREADS panel offers "boot" on a Dead row (src/Game/DevConsole_Threads.cpp:140-144, :184). Boot calls Restart, which relaunches the stored job, so ComputeBucket runs against a freed director and the game crashes.

The registry also gains four dead `ai.bucketN` slots each time this happens, and only `threadreap` clears them. The verifier narrowed the trigger: it is a switch to a different world inside one process (LoadWorld), not a trip to the title and back.

Fix: add `Manager::Remove(id)` and call it from `~AsyncDirector` instead of Stop, so a worker's job can never outlive the object it captures.

### C55. Monster archetypes are an enum tested in many places, not one class per behaviour
*medium - structure - effort L*

Brief (C) asks for each behaviour to be its own class, the way spells and effects are. `ai::Archetype` is a bare enum (src/Game/MonsterAI.h:84), and its rules are split across two threads:

- Brain::Think, on the worker threads, holds lurker dormancy, the sentry's cone and the kite choice (src/Game/MonsterAI.cpp:122, :135, :157).
- The main thread decides swarm's all-round sensing while it builds the snapshot (src/Game/DungeonWorld.cpp:2152).
- The threat profile and the editor forms test the enum again (see C56).

The verifiers narrowed this. The real difference between archetypes comes to about four flags. The executors on the main thread choose by Intent::Mode, not by archetype (src/Game/DungeonWorld.cpp:1313-1331), and they are shared on purpose.

A related bug: ProvokeMonster forces Engage (src/Game/DungeonWorld.cpp:2239). Engage needs a path, and a kiter or fleer has none, so a struck one stands still until its next think, which can take up to about 2 s.

Fix: after C56's table, add stateless `ai::Behaviour` classes registered like AllSpells. They should own only the thinking side: perception and choosing a mode. Keep the executors shared per mode, because moving them into the classes would break the split between thinking and acting.

Against a documented decision: docs/ai.md Layer 1 chose the enum plus "an executor per mode".

### C56. The archetype names, the "kites" test and the behaviour form are each repeated in several places
*medium - cleanup - effort S*

- **Name list:** the six archetype names are typed out four times (src/Game/DungeonWorld_LevelIO.cpp:371, src/Game/Game_Editor.cpp:1243, src/Game/EntityInspector.cpp:16, src/Game/MonsterConfigDialog.cpp:23), and ParseArchetype repeats them as an if-chain (src/Game/DungeonWorld_Load.cpp:81). Nothing checks any of them against the enum. Two writers index `kArch[int(enum)]`, so if a new archetype is missed in one list, that writer reads past the end of the array.
- **"Kites" test:** `Skirmisher || Caster` appears five times.
- **Behaviour form:** it is built twice, line for line (EntityInspector.cpp:70-105 and MonsterConfigDialog.cpp:131-167).

Fix: put one table in MonsterAI.h (name, loc key, kites, usesSpell) with Name/Parse helpers and a static_assert, as LightProfile.cpp already does. Build both dialogs' behaviour rows from one shared builder.

Against a documented decision: CLAUDE.md lets MonsterConfigDialog own the archetype rows outside the schema. EntityInspector has since copied them, so that exception now produces two forms.

### C66. The AI snapshot pool grows during play, so an allocation can land in a guarded frame at random
*medium - rule - effort S*

BuildAISnapshot reuses a pooled snapshot that nobody holds. If none is free, it creates a new one, which allocates the map-sized `blocked` and `occ` grids and the monster list (src/Game/DungeonWorld.cpp:2099-2114, :2155). Nothing fills the pool in advance. Each worker holds a snapshot for its whole tick (src/Game/MonsterAI.cpp:313) and runs at below-normal priority. So whether the pool needs one more snapshot depends on thread timing, and that can first happen minutes into play, inside a guarded frame. AllocTest only catches it by luck. The walkable-grid pool keeps one spare but grows the same way when two map changes come close together (src/Game/DungeonWorld.cpp:2071-2079).

Fix: at level load, fill both pools to `kBucketCount + 2` (the published snapshot, one per worker, and the one being built), with grids sized to the map. Log a warning if either pool ever grows.

### C71. The formation list and the blast list are never reserved, so the first fight allocates
*medium - rule - effort S*

AssignFormation refills `m_formationScratch` every frame (src/Game/DungeonWorld.cpp:1965-1971), but nothing reserves its capacity. The first monster to notice the party, and every new peak in the number of aware monsters, grows it inside a guarded frame. `m_activeBlasts` has the same gap: the first blast adds an element of several KB (src/Game/DungeonWorld_Combat.cpp:2652).

The verifier narrowed this. These are one-off high-water growths, a few allocations per world from one logged stack, which is the kind AllocTest.ps1 counts as warm-up. They still break the "reserved up front" rule, and the guard fires on the first fight of every session.

Fix: reserve in MakeMonster (src/Game/DungeonWorld_Load.cpp:1047), which already reserves `aiPath` and `effects`. Make `m_activeBlasts` a fixed array like `m_pendingBolts`, and correct the "no per-frame allocation" comment.

### C59. A caster given a non-bolt spell quietly shoots ember bolts, with no warning
*low - authoring gap - effort S*

A caster whose spell cannot be thrown (a ward, a light, a hand spell), or whose spell id does not exist, falls back to the plain ember bolt (src/Game/DungeonWorld_Combat.cpp:1755-1775). The verifier confirmed the fallback is deliberate. The gap is that nothing tells the author:

- the load warns only when the spell is empty (src/Game/DungeonWorld_Load.cpp:704);
- no validator check covers it;
- both dropdowns list every spell.

Fix: warn at load and in the checker, and limit the dropdowns to bolt spells.

### C68. A saved monster loses its facing and its place on its patrol
*low - bug - effort S*

The monster save writes no facing (src/Game/DungeonWorld_Save.cpp:225-237), and `patrolIdx` is never saved (src/Game/DungeonWorld.h:2385). After a reload, a patroller faces its spawn direction, so its sight cone moves, and it restarts at waypoint 0 (src/Game/DungeonWorld_Combat.cpp:1709). The save format (src/Game/SaveGame.h:197) also has no slot for state a future behaviour might need to keep.

Fix: save facing and `patrolIdx`, and give the future Behaviour base its own save line.

### C73. Choosing an animation clip uses the combat random-number stream
*low - bug - effort S*

PickClip draws from `m_combatRng` (src/Game/DungeonWorld.cpp:1440) whenever a monster state has more than one clip. So adding a purely cosmetic attack or hit clip changes every later combat roll in a seeded eval sweep, and the sweeps are promised to compare sample for sample (docs/eval-harness.md:320-322).

Fix: give cosmetic choices their own random stream, as src/Game/Projectiles.h:472 already does.

### C74. Monsters, dropped items and thrown items treat pits and stair holes as solid floor
*low - bug - effort S*

- **Monsters:** FreeSlotInCell and the snapshot's walkable grid never check for stairs or floor holes (src/Game/DungeonWorld.cpp:2313-2322, :2084-2087), so a monster can stand over the pit the party falls through.
- **Drops:** DropItemAt accepts a pit and a shut door's square (src/Game/DungeonWorld_Load.cpp:1653).
- **Throws:** LandThrown refuses the door's square but not a hole (src/Game/DungeonWorld_Throw.cpp:281).

So items can hang in the air over a shaft.

Fix: one shared "can this rest here" check used by all of these. On a hole, either refuse or drop the item to the level below.

### C61. Small dead code and wrong comments in the AI
*low - cleanup - effort S*

- `m_nextGroupId` is written but never read (src/Game/DungeonWorld.cpp:1894).
- ProvokeMonster writes `intent.targetX/Z` (src/Game/DungeonWorld.cpp:2240), which nothing on the main thread reads.
- The comment on `Intent::targetX` is wrong (src/Game/MonsterAI.h:171).
- The `selfId` parameter is unused.
- The comments for ParseArchetype and ConsumeAIPlans sit on the wrong functions (src/Game/DungeonWorld_Load.cpp:64, src/Game/DungeonWorld.cpp:2178).
- DungeonWorld.h:2260 names `MonsterShoot`, which no longer exists.
- "Within aggro range" is measured two ways: Manhattan distance at src/Game/DungeonWorld.cpp:1110, Chebyshev in Think.

### C67. Run, Flee and Defend animations can be authored but never play
*low - cleanup - effort S*

DesiredState (src/Game/DungeonWorld.cpp:1413-1426) never returns Run, Flee or Defend. The catalogs, the import folders and the config dialog all accept clips for those states, and the skeleton kit's anim_run and anim_defend clips never play. A fleeing monster plays Walk. Now that the AI has a Flee mode, the comment at src/Animation/CreatureState.h:9-12 saying no behaviour uses these states is out of date.

Fix: have the Flee mode pick the Flee clip when the model has one, or hide the unused states from the authoring screens.

### C70. AI buffer pools use `use_count()==1` as the hand-back signal
*low - rule - effort S*

The snapshot, grid and plan pools decide a buffer is free when `use_count() == 1`. The C++ standard gives that check no ordering guarantee. A verifier found this is a conformance point only: MSVC releases a shared_ptr with an interlocked full barrier, so there is no real race on this toolchain. A force-killed worker also leaves one buffer per pool stuck, which is negligible.

Where: src/Game/DungeonWorld.cpp:2060-2104, src/Game/MonsterAI.cpp:320-329.

Fix: add a comment explaining why the pattern is safe on MSVC. If you want it portable, use a per-buffer atomic in-use flag with release/acquire.

Against a documented decision: CLAUDE.md describes the `use_count()==1` reuse pattern as the design.

### C72. The threat lock rule is written out twice
*low - cleanup - effort S*

`UpdateThreatLock` and `ThreatTarget` both compute the threshold, both test "is the lock still held" (in range, alive, above threshold), and both run the same alive-and-above-threshold argmax loop. A rule change made in only one of them would leave the lock and the target disagreeing.

Where: src/Game/DungeonWorld.cpp:2256-2267, src/Game/DungeonWorld.cpp:2292-2304.

Fix: move the shared code into `ThreatHeld` and `ThreatArgmax` helpers and call them from both functions.

## 3. Dialogs and inspectors

### C101. The type editor shows no control for damage-type fields
*high - bug - effort S*

Commit 542de729 moved monsters.cat `dmgtype` and effects.cat `damage_type` from `Enum` to the new `FieldKind::DamageType`. It updated the schema and `optionsFor` but not the dialog. `TypeEditorDialog::BuildUI` switches on `spec.kind` with no `DamageType` case and no `default:`, so both rows build no widget. As a result:

- A monster's damage type and a DoT's resisted type cannot be authored in the editor.
- The `?` help still lists both fields.
- The `optionsFor` case is dead code.

Nothing is corrupted, because only touched fields are written, and the .cat can still be hand-edited. Nothing warned because /W4 does not include C4062.

- src/Game/TypeEditorDialog.cpp:339 (the switch), :422-430 (Enum/CatalogRef branch)
- src/Game/CatalogSchema.cpp:251, :648
- src/Game/Game_Wiring.cpp:473

Fix:
- Add `case FieldKind::DamageType:` beside `Enum`/`CatalogRef`.
- Make `nullable` (line 428) false for it, as it is for Enum. The `values` line already sends it to `optionsFor`.
- Enable `/we4062` for the Game lib, with no `default:` (one would silence the warning), so the next new FieldKind fails the build.

### C75. Editor dialogs and inspectors have no shared modal base
*high - structure - effort L*

Brief item B4 (one base for editor dialogs and inspectors) is not met. Only the 7 instance inspectors share a base. InstanceInspector mixes the modal shell with parts only an instance needs (facing strip, a forced Save footer), so ProjectileInspector, InspectPicker and 13 other dialogs stand alone. Each one repeats:

- the ctor (`CloseIcon`, `kDialogTextScale`): 16 sites;
- the `clamp(h * 0.020, 12, 24)` font: 15 sites;
- the dim + panel backing: 16 sites;
- its own open and rebuild flags.

The card itself is already shared through `BuildDialogChrome`, so each copy is 15-20 lines of shell.

The copies have drifted:
- TypeEditorDialog hand-rolls the help word-wrap that `DrawHelpOverlay` replaces (src/Game/TypeEditorDialog.cpp:709).
- Footers sit left, centred or right. CLAUDE.md's "right-aligned" is out of date.
- No dialog checks `PopupOpen()` before Esc. Pressing Esc to close a drop-down in an inspector or Level settings reverts every edit and closes the dialog (src/Game/InstanceInspector.cpp:180, src/Game/LevelSettingsDialog.cpp:259).

Fix: add a ModalDialog base under InstanceInspector. It owns the context, font, backing, rebuild flag, help overlay and a virtual `OnEscape()` that closes an open popup first. Subclasses supply `BuildContent(DialogChrome&)`. The game-side dialogs can move later.

Against a documented decision: src/Game/DialogLayout.h:22 keeps the panel backing out of the widget tree.

### C76. Dialog routing is hand-kept in parallel lists that already disagree
*high - structure - effort M*

Game names each dialog separately in five places:
- the Playing input chain (src/Game/Game.cpp:2282);
- a WorldMap chain (:2189);
- the draw list (:2906);
- the preview chain (:2680);
- UnloadWorld's 20 `Close()` calls (:583).

src/Game/Game.h:1215 argues for "ONE list" but applies it only to the inspectors. Special cases are patched by hand (`m_typeOverBalance`, and Generate gated on Validate).

The lists disagree:
- NewWorld, AssetDialog and WorldSettings take input before dialogs drawn over them.
- Render draws every dialog in every state, but Update routes them only in Playing or WorldMap. So the console commands `worlds newdialog`, `dungeons dialog <id>` and `assetpicker` can leave a dialog on screen that ignores all input.

Every combination the mouse can reach is consistent today; the mismatches need the console.

GameUI does the same for item details and the portrait picker at about ten sites. One bug comes from this: open an item's details, then type `editor` in the console. The details card stays drawn over the editor, with an empty preview and no input, because Game.cpp:2879 has no editorMap guard.

Fix: keep one ordered list of C75's base class:
- Update the topmost open dialog first and draw bottom-up.
- Close them all in one loop.
- Give each dialog a mask of allowed states, and refuse an open outside them.
- Add `Tick`/`OnClosed` hooks for the special cases.

Unify the preview chain later.

### C77. Worlds and World settings dialogs open over a dungeon and cannot be closed
*medium - bug - effort S*

Their Update runs only in `AppState::WorldMap` (src/Game/Game.cpp:2189), but Render draws them in every state (:2933). Inside a dungeon, the map overlay's world page keeps its editor toolbar, because editor-on-arrival sets Editor mode (:1531) and nothing resets it. That toolbar and the right-click open both dialogs with no state check (src/Game/Game_Wiring.cpp:343, src/Game/WorldMapView.cpp:226).

The dialog then sits over the HUD and cannot be closed until the player returns to the world map. The console already guards this case (src/Game/Game_DevWorld.cpp:1082). Fix: route both dialogs in the Playing chain, or hide the toolbar in the overlay.

### C78. With the console open, the world runs through the editor pause, dialogs and exit prompt
*medium - bug - effort S*

The console branch (src/Game/Game.cpp:2035) updates the world in Playing and returns before every freeze the Playing path applies: the exit prompt (:2272), the editor dialogs (:2276) and the editor pause (:2423). While the console is open, monsters act in a paused editor or during "Leave the Crypt?".

`MapView::SetMode` also clears the pause even when the mode does not change (src/Game/MapView.h:153). So bare `editor` and its subcommands unpause the editor, and it stays unpaused (src/Game/Game_DevCommands.cpp:297, :658).

Fix:
- Use one world-frozen decision in both branches.
- In SetMode, write `if (mode != m_mode) m_editorPaused = false;`.

Verifier narrowing: this only happens with the dev console or the harness. The PropInspector handle risk does not follow.

### C79. The world map overlay keeps Editor mode, so its dialogs freeze and its paint strokes never commit
*medium - bug - effort S*

Once WorldMapView is put into Editor mode, nothing switches it back to Play. Two routes do that: the landing page's Editor entry on a world that starts outside (src/Game/Game.cpp:1531), or `worldedit on`. After that, the world page on the M map shows the editor toolbar, paints, inspects and draws with the fog off.

Its Settings and Worlds discs and a doorway right-click all open dialogs. These are drawn in every state, but only the WorldMap host updates them (Game.cpp:2488 vs 2189). So they freeze on screen, and Esc closes the map underneath instead.

A paint opens an undo step that only the WorldMap host commits (Game.cpp:2226). The next dungeon edit overwrites that step, so the terrain change can't be undone. Painting needs a terrain armed from the console.

Fix: make `Editing()` return `m_mode == Editor && !m_overlay`, and gate the toolbar on it (src/Game/WorldMapView.cpp:57, 216, 226). This only affects authors; no saved data is damaged.

### C80. Finishing a patrol route can reopen the inspector on the wrong monster
*medium - bug - effort S*

"Edit route" closes the inspector, but a stationary right-click still inspects while a route is being laid (src/Game/MapView.cpp:794 has no LayingRoute gate). Every monster inspect also overwrites the shared `m_inspectCfg` / `m_inspectPreview` (src/Game/Game_Inspect.cpp:54).

Repro: inspect A, Edit route, right-click B, Esc, Enter. The inspector reopens on B but shows A's waypoint count, and Save / Apply / Edit route then act on B (src/Game/Game.cpp:2455).

Fix: reopen through `OpenInspectorFor` with `m_mapEditor.RouteId()`, which re-queries the monster and rebuilds its preview. Then drop the reopen caches.

### C81. Esc is inconsistent: one dialog ignores it, most act on it too eagerly
*medium - bug - effort S*

- **The asset-creation dialog has no Esc at all.** Game returns right after its Update (src/Game/AssetDialog.cpp:430, src/Game/Game.cpp:2292), so it can't be closed from the keyboard. This breaks CLAUDE.md's close convention.
- **Esc meant for a drop-down or colour picker closes the whole dialog.** The inspectors and the Level, New world and Generate dialogs check Esc before their tree runs and never ask `PopupOpen()` (src/Game/InstanceInspector.cpp:180, src/Game/LevelSettingsDialog.cpp:259). In those dialogs, Esc reverts every live edit and closes. World settings only closes, since it has nothing to revert.
- **The editor ladders skip steps.** The dungeon editor's ladder skips an open level dropdown, so Esc closes the whole editor (src/Game/MapView.cpp:640). The world editor pauses even with a terrain brush armed (Game.cpp:2211).

Fix:
- Gate each dialog's Esc on `!m_ui.PopupOpen()`.
- Give AssetDialog an Esc.
- Add the dropdown and a Disarm step to the ladders.

### C89. The asset-creation dialog still uses the legacy owned-font UIContext
*medium - bug - effort S*

AssetDialog is the last caller of the pre-FontLibrary UIContext constructor (src/Game/AssetDialog.cpp:44). It draws in Consolas at a fixed 18 px. In that form `FontAt` ignores role and size (src/UI/UIContext.cpp:38), so the dialog's text and title scales do nothing.

At 900p it draws 18 px text where every other dialog draws 36 px, inside rows sized for the larger text, and it never follows the window height. It is also the only thing keeping `Font::SetHeight` (no callers) and its re-raster branch alive (src/UI/Font.cpp:94, 124). CLAUDE.md's "Font::SetHeight re-bakes the atlas" line is out of date.

Fix: build it as `UIContext(fonts, FontRole::Body, 18.0f)` and call UseFont like the other dialogs. Then delete the owned-font constructor, `m_ownedFont`, SetHeight and the re-raster branch.

### C91. The Yes/No confirm is placed by window fractions, and SlotList draws a second one
*medium - bug - effort M*

**OpenConfirm** places a Panel, Labels and Buttons as root siblings at hand-stepped window fractions (src/Game/GameUI.cpp:1424-1449). This is the pattern CLAUDE.md warns about.
- Its body is a non-wrapping Label 448 px wide at 1600x900.
- `confirm.restart.body` is estimated at about 750 px (not rendered), so it very likely runs off the card.
- No command or sweep opens this modal, so uioverlap has never checked it.

**SlotList** hand-draws its own delete confirm in the overlay pass (src/UI/Controls.cpp:1856-1936). It has no widgets and no keyboard, and on the saves page Esc leaves the page instead of closing it.

**InspectPicker** also sizes its card in fractions around rem rows (src/Game/InspectPicker.cpp:20).

Fix: build one widget-based confirm with a wrapped body in a Stack and Yes/No buttons. SlotList raises it through a callback. Add a dev hook so the UI sweep reaches it.

### C96. The character sheet's layout assumes a 16:9 window
*medium - bug - effort M*

The sheet window takes the window's aspect ratio (src/Game/GameUI.cpp:1182), but its sockets are panel fractions tuned for 16:9 (src/Game/CharacterSheetLayout.h:34, 88). A pack cell comes out at:
- 88x86 px at 1920x1080
- 118x86 px at 2560x1080
- 88x96 px at 1920x1200

So the sockets, portrait and tab stones stretch on ultrawide and 16:10 screens. Card mode's em constants (src/Game/CharacterSheet.h:200) were copied by hand from 1920x1080 and will drift if any fraction moves.

Fix: size the sheet in em like `PartyWindow::SizeForEm` (or pin its aspect), and derive the card numbers from the same values.

### C99. A monster switched to Caster shows a spell that is never saved
*medium - bug - effort S*

The behaviour form is copied between src/Game/MonsterConfigDialog.cpp:131 and src/Game/EntityInspector.cpp:71. A third copy of the archetype table sits at src/Game/Game_Editor.cpp:1243.

Both copies default the spell dropdown to entry 0 when `m_cfg.spell` matches nothing. So it shows a spell while the config stays empty. The writer skips an empty spell (Game_Editor.cpp:1249), and the next load warns (src/Game/DungeonWorld_Load.cpp:704). Both the type and the per-instance override are affected.

Fix: share one behaviour-rows builder. When Caster is picked, set the spell to the first offered one, or offer a "(none)" row.

### C100. Renaming a quest stage through another stage's id wipes that stage's log line
*medium - bug - effort S*

The stage id field moves `text_<id>` to the new id on every keystroke, with no uniqueness check (src/Game/TypeEditorDialog.cpp:125-140). New stages are named `stage<N>`.

Example: editing `stage2` to `stage10` passes through `stage1`. That keystroke overwrites stage1's line with stage2's. The next keystroke moves it to `text_stage10` and clears `text_stage1`, so stage1's text is gone, and a save removes the empty field.

Fix: don't apply a typed id that matches another stage, say so in the notice row, and only move the text once the id is unique.

### C102. World settings' status line points at the wrong tab when a doorway is selected
*medium - bug - effort S*

BuildUI builds all three tabs. When a doorway is selected, the Doorways tab takes over the single `m_noteLabel` that the World tab set (src/Game/WorldSettingsDialog.cpp:220, 572).

- Opened from a doorway, the Doorways status row shows the World tab's start-terrain caption.
- A refused start position on the World tab ("impassable" / "offgrid") is written into the hidden Doorways label. The World caption stays stale, and it is the only feedback for that refusal.
- Switching tabs never rebuilds, so the "switching tabs is a fresh start" comment (line 191) is wrong.

Fix: one note label per tab, with SetNote writing to the tab that raised it.

### C85. Form-field helpers are copied across dialogs, and RenameType checks nothing
*medium - cleanup - effort S*

- **The record-id character rule** (letters, digits, `_`, `-`) is written out 11 times. Three of the copies are identical `FilterId` functions (src/Game/NewWorldDialog.cpp:29, WorldsDialog.cpp:26, WorldSettingsDialog.cpp:70). LevelSettingsDialog.cpp:178 (tags, allows a space) and Game_Generate.cpp:445 are deliberate variants.
- **AddNumericField** exists three times (src/Game/BalanceDialog.cpp:35, LevelSettingsDialog.cpp:41, WorldSettingsDialog.cpp:36) and has already drifted (maxLength 10 vs 8, return type). The note label is copied three times, and ten dialogs repeat the deferred `m_uiRebuild` flush.
- **Game::RenameType** checks only empty, unchanged and duplicate ids (src/Game/Game_Editor.cpp:938). From the console, `typeset rename ... x]y` writes a header that reloads as `x` while the records say `x]y`, so the type is missing on the next launch. The dialogs filter this; the console doesn't.

Fix:
- One `IsRecordIdChar` / `FilterRecordId` beside the record tokenizer, plus a TextField filter hook.
- NumericField and NoteRow helpers in DialogLayout.
- Valid-id checks inside RenameType and the other core rename/create APIs.

### C86. Three word-wrap implementations; the type editor's help cuts fields off
*medium - cleanup - effort S*

`ui::WrapLines` (src/UI/TextWrap.h:21) is the shared, allocation-free wrap. Two places roll their own anyway, each with a different card and width:
- **DrawHelpOverlay** (src/Game/DialogLayout.cpp:133) builds a string every frame, which contradicts its own comment. It is editor-only, so no allocation guard is broken.
- **The type editor's "?" overlay** (src/Game/TypeEditorDialog.cpp:709) stops when the card is full and has no scroll. On the monster Stats tab the last fields (reach, attackcd, movecd, power) lose their help.

The help-input swallow is also copied three times, and the busy overlay twice.

Fix: build DrawHelpOverlay on WrapLines, taking heading + paragraph items that scroll or shrink, and use it in the type editor. Share one busy overlay.

### C88. The click-to-rename title is implemented twice
*medium - cleanup - effort M*

TypeEditorDialog (src/Game/TypeEditorDialog.cpp:258, 666) and LevelSettingsDialog (src/Game/LevelSettingsDialog.cpp:101, 259) each have their own copy of the rename editing logic. That covers the title-slot text field, focus, the id filter, Enter-commits-or-stays-open, Esc and click-away cancel, and the deferred rebuild. EditableTitle (src/Game/DialogLayout.h:57) shares only the display part.

The two copies have already drifted: the type editor reports a refused rename, the level dialog says nothing. Their maxLength differs too (32 vs 24), which may be deliberate since a level stem is a file name.

Fix: grow EditableTitle into a RenameTitle widget that owns the edit mode, filter, cancel rules and an `onCommit(old, new, problem&)` callback.

### C94. The tooltip face is hand-drawn at about ten sites in three looks, and the shared one is private
*medium - cleanup - effort M*

`ui::DrawTooltip` lives in an anonymous namespace (src/UI/Controls.cpp:193-211), so other code cannot call it and each site paints its own tip box. Placement is already shared through `ui::PlaceTooltip`. What gets copied is the face, padding and text offset, and there are three different faces:

- **The theme's panel at 0.97:** Controls.cpp, used by Button and DropDown.
- **A hard-coded `{0.10, 0.10, 0.13, 0.97}`:** HandSlot.cpp:157-167, DrawRuneTip at src/Game/PartyHudDraw.cpp:227-237 (a line-for-line copy), FloatingPanel.cpp:343-354, the armour tip, MapView_Issues.cpp:131 and DevConsole_Profile.cpp:1289. Settings -> UI Theme Colors cannot reach any of these.
- **The stone `DrawPanelFace`:** EffectIcon and NameTag (src/Game/CharacterPanel.cpp:148, :217).

The editor tips use `kMapBg` (MapView, WorldMapView, MapEditor_Categories.cpp:306 and :325). That may be a deliberate flat editor look.

The result is that a hand box, a rune, an effect icon and a name each show a different tip face in the same HUD. The armour, map-issue and profile tips are multi-row, so they share only the box face, not the text layout.

Fix: make one public `ui::DrawTextTip(ctx, batch, font, text, anchor, side, align)` with a single theme-driven face, plus a `DrawTipFace(rect)` for the multi-row tips, and route every site through them.

### C95. The sheet and the party-window cards duplicate the status band, the trim helper and the service wiring
*medium - cleanup - effort M*

The status line is drawn twice:
- **The sheet** uses a private `FitWidth` that binary-searches and writes "..." (src/Game/CharacterSheet_Status.cpp:51, used at :230/:236).
- **PartyWindow** uses `ui::FitText` / `DrawFittedText` with ".." (src/Game/PartyWindow.cpp:148-170). CLAUDE.md names that as the one trim helper.

So the same hover trims differently on a sheet and on a card. PartyWindow also measures the trimmed name without its mark, so the status text's ".." can draw over the name's ".." (PartyWindow.cpp:166). The status constants are duplicated as well (CharacterSheetLayout.h:54 and PartyWindow.cpp:18).

In GameUI, the sheet's etch arrays and its onRejectDrop / onRejectHold / defenseFor / defenseWith / spells lambdas (src/Game/GameUI.cpp:1197-1239) are retyped for each card (1813-1847). They have already drifted: the HUD hand's refusal (GameUI_Items.cpp:85) logs `log.cant_hold` without the bump sound the sheet plays.

Fix: delete `FitWidth` and draw the sheet's line through `DrawFittedText`. Add a `WireSheet(sheet, member, menu)` helper plus `RejectDrop` / `RejectHold` methods.

A verifier rates the wiring half low on its own: it is about 25 lines of delegating lambdas in one function.

### C97. ProjectileInspector is a standalone modal outside the inspector base, and its damage row means two things
*medium - structure - effort L*

ProjectileInspector has no base class (src/Game/ProjectileInspector.h:8-10, :30). It gets its own branches in Game's Update (src/Game/Game.cpp:2395) and Render (2958), and it hand-draws its dim, panel and border (ProjectileInspector.cpp:101-104), a pattern found in about 15 dialog files.

More broadly, none of the 13 editor dialogs shares a base. Game routes them by hand through an Update if-ladder (Game.cpp:2188-2395) and a Render ladder in a different order (2903-2963). MonsterConfigDialog keeps a parallel preview state, `m_previewMon*` (Game.cpp:2340-2367), beside PreviewSpec, and `m_previewMonMat` is written at :2353 and never read.

There is also a bug in the damage row. Game_Inspect.cpp:270 shows `p.atk.damage`, which is before potency for a bolt (applied at impact) but after it for a throw (Throw.cpp:83). The same row means two different things.

Fix: add a modal base and a stack that Game walks once per pass, make the facing strip optional, and fold the monster preview into PreviewSpec. Show the potent damage, or label the row.

Against a documented decision: CLAUDE.md and the header describe the inspector as standalone on purpose, because the base's facing/placement strip does not fit a projectile.

### C98. The world's inspector seam is ad hoc per kind
*medium - structure - effort L*

DungeonWorld talks to the inspectors in many shapes:
- **Two handle types for one idea:** MoveTarget addresses an item by its live vector slot (src/Game/DungeonWorld.h:1824), while Game::InspectTarget uses its stable entity id (src/Game/Game.h:1174).
- **Four preview shapes, five removal keys:** previews come back as vectors, two different structs, or a filled span. Removals take an index, entity id, runtimeId, cell, or cell plus wall.
- **Settings in two styles:** monsters use a 9-out-param list, while doors and buttons use Edit structs.
- **Torch and brazier twins:** the Torch/Brazier Settings/TypeAt pairs differ only by `wall` (DungeonWorld.cpp:1664-1721), although FindFire's `wall < 0` already covers both.
- **Dead code:** `MonsterInstanceAt` (DungeonWorld.cpp:1526) has no caller.

DoorInspector::Config repeats DoorEdit's ten fields, and ButtonInspector::Config does the same with ButtonEdit. Both are copied one by one in each direction (src/Game/Game_Inspect.cpp:124-133, src/Game/Game_Wiring.cpp:959-968). A new door field needs four edits, and a missed copy-back silently drops the edit.

Fix: hold the Edit struct inside the Config, unify the twins on `wall = -1`, and move toward one `PlacedRef` key for inspect, move and erase. `seconds` still needs translating, because it means different things on the two sides.

A verifier narrowed the scope: all seven inspectors already derive from InstanceInspector, so this is the world-side glue, not a missing UI base.

### C103. The "keep a stale value selectable" drop-down is hand-coded 5 times and missing from about 8 more
*medium - cleanup - effort M*

`FlagDropDown` (src/Game/InstanceInspector.cpp:17-40) is already the general form: a None row, the choices, and a stale value appended so it stays selected. It hard-codes the flag label, though, so the rule is retyped in ButtonInspector::Open, StairInspector::Open, LevelSettingsDialog.cpp:192 and GenerateDialog.cpp:146.

It is missing in these places, each of which shows something other than what the record holds:
- **Door key:** shows "No key" (Game_Inspect.cpp:136 lists only `category=key` items).
- **Door opener:** shows the default.
- **Niche type and caster spell:** show the first entry.
- **WorldSettingsDialog's `IndexOf`:** its comment promises a row, but it returns 0 (WorldSettingsDialog.cpp:59-66).

The real value survives only until someone touches the drop-down.

Fix: generalise FlagDropDown into a `ChoiceDropDown` in DialogLayout.h with an optional none-label, use it at all these sites, and delete `IndexOf`.

### C109. A party spec's shown name, face and colour are worked out in about seven places, and one copy has drifted
*medium - cleanup - effort S*

`Game::BuildMember` already applies the premade rule: an empty name keeps the premade member's name, and the same goes for portrait and colour (src/Game/Game_Party.cpp:97, 196-199). PartyCreationPage re-derives that rule separately in its refusal check, its status lines, RefreshPreviews, RefreshFaces, BuildIdentity, and SetRace (src/Game/PartyCreationPage.cpp:226, 352, 386, 409, 420, 526, 553), and again in GameUI_Party.cpp:122.

The member slot's copy dropped the premade case (PartyCreationPage.cpp:113-115). If you clear a premade member's name field, the slot reads "New member" while the name field and the started game use the premade name. SetName accepts the empty string, so this happens in normal use.

Fix: add `ShownName` / `ShownPortrait` / `ShownColor(i)` on the page, read from the preview Character where one exists, and call them everywhere, including the portrait picker.

### C90. Editor dialogs paint their panel in the user's theme but their widgets in the default theme
*low - bug - effort S*

Most editor dialogs use the theme only for the backing rect in Render (e.g. src/Game/TypeEditorDialog.cpp:691). Their own `m_ui` contexts never get `SetTheme`, so buttons, labels and drop-downs draw with `ui::Theme{}` defaults. The only `SetTheme` callers are AssetDialog.cpp:115, AssetPicker.cpp:525 and GameUI.cpp:262. Changing Settings -> UI Theme Colors recolours a dialog's frame but not its contents.

Fix: push the theme into each dialog's context when it changes, beside `GameUI::ApplyTheme`, and drop the per-call theme parameter from Render.

### C104. Clearing a patrol route throws EntityInspector back to its AI tab
*low - bug - effort S*

EntityInspector builds a fresh TabControl on every rebuild and never restores the active tab (src/Game/EntityInspector.cpp:52-56). "Clear route" on the Patrol tab calls `RequestRebuild()` (:127-131), so the dialog jumps to the AI tab and the user never sees the waypoint count change.

GenerateDialog.cpp:99 and WorldSettingsDialog.cpp:700 already save the tab across a rebuild.

Fix: capture `m_tabs->ActiveTab()` before the rebuild and restore it, ideally in the InstanceInspector base.

### C107. Inspector and InspectAt text hard-codes English and bypasses Loc
*low - bug - effort S*

Some user-facing text never goes through Loc:

- **ProjectileInspector rows:** units are formatted directly as "pts", "m/s" and "m" (src/Game/ProjectileInspector.cpp:60-62).
- **ProjectileInspector payload line:** built with `" {:.1f}/s for {:.1f}s"` and `" ({:.0f}%)"` (src/Game/Game_Inspect.cpp:287-290).
- **The editor's cell message:** passes the English words "wall" / "floor" and English plurals ("2 monsters", "1 prop") into a localised key (src/Game/MapEditor.cpp:1260-1272). A German editor reads "Zelle 3, 4: wall, 2 monsters".

This breaks CLAUDE.md's rule that all user-facing text goes through Core/Loc. Showing the raw effect id is a documented choice; the words around it are not.

Fix: add `map.proj.*` and `map.select.*` keys to all five lang files and format through `loc::Format`.

### C111. ThumbCache eviction does not protect tiles that are on screen
*low - bug - effort S*

ThumbCache promises never to evict "this frame's entries" (src/Game/ThumbCache.h:13-14). But Update calls `Tick()` before `Evict()`, while visible tiles were stamped by the previous Draw. `BeginLoad` also returns early on entries it has already tried without restamping them (ThumbCache.h:57, :73). So only fresh loads are protected, and visible tiles survive only because they are the newest.

This shows up only past about 180 visible tiles with the cache over its 240 cap, which means a 5K or ultrawide-4K window. On a normal 4K screen it stays latent. When it hits, tiles blank and reload with GPU drains. AssetPicker uses the same Tick / Update / Evict order.

Fix: restamp in `BeginLoad` or Touch the visible ids in Update, and size the cap from the visible count.

### C115. A pending Yes/No prompt survives a console-driven restart and answers against the new game
*low - bug - effort S*

Only `ResolveConfirm` clears `m_confirmActive` (src/Game/GameUI.cpp:1456). UnloadWorld and StartNewGame never cancel it, so after `worlds load <x>`, `newgame`, `load` or an eval `reset`, the old question stays up and blocks play (src/Game/Game.cpp:2207, 2272). Answering it runs the stored `EnterLocation` / `LeaveDungeon` closure with the old world's ids (src/Game/Game_World.cpp:367, 377). The question is still shown first, but it names the previous world's dungeon.

This is reachable only through the dev console, because the prompt takes all other input.

Fix: add a `CancelConfirm` and call it in UnloadWorld and wherever a game begins (StartNewGame / LoadGame).

### C82. Console-open checks after the console's early return are dead
*low - cleanup - effort S*

Game::Update returns for every non-loading state while the console is open (src/Game/Game.cpp:2035-2043). The WorldMap and Playing branches below still test `!m_console.IsOpen()` six times (2208, 2211, 2240, 2251, 2253, 2273), and those checks can never be false. They suggest the console can be open there, which is the wrong mental model.

Fix: delete them and keep the rule in the one early return.

### C83. The asset-picker lambda is wired twice, and a few other small helpers are copy-pasted
*low - cleanup - effort S*

- **Picker lambda:** the same lambda body is assigned to `m_typeDialog.onPickAsset` and `m_assetDialog.onPickAsset` (src/Game/Game_Wiring.cpp:596, :606). The `assetpicker` dev command has a third near-copy (src/Game/Game_DevWorld.cpp:1107). One `Game::OpenAssetPicker(textures, current, apply)` would replace all three.
- **Bool display:** src/Game/TypeEditorDialog.cpp:341 treats only `1`/`true` as true. `serialize::GetBool` (src/Game/Serialize.cpp:44) accepts anything that does not start with 0, f or F. So a hand-written `yes` loads as true but shows unchecked.
- **Other copies:**
  - The apply-then-save pairs (src/Game/Game.cpp:203, Game_Wiring.cpp:774).
  - StartNewGame inlines `DungeonWorld::FirstLevel` twice (Game.cpp:1103).
  - `Project::List` + `std::find` appears four times.
  - `Vec4 Mix` is copied twice.

### C84. The inspector Delete lambda is pasted eight times and says "removed" even when nothing was
*low - cleanup - effort S*

Every onDelete in src/Game/Game_Inspect.cpp (74, 161, 187, 206, 249, 318, 356, 388) does the same three things: Begin the undo step, Commit with the remove, then print `map.erase.removed`. It prints that line even when the remove returns false.

- **Fix:** add one `DeleteVia(remove)` helper that messages only on success.
- **Keep the stair exception:** `RemoveStairAt` sends its own message naming the paired level (src/Game/DungeonWorld_Remote.cpp:241). The stair's missing line is deliberate, not drift.
- **Related:** door and button settings are copied field by field in both directions (Game_Inspect.cpp:124, Game_Wiring.cpp:958). Share `DoorEdit`/`ButtonEdit` through a small header, not DungeonWorld.h.

### C87. TypeEditorDialog and WorldsDialog both re-check the typed-delete match
*low - cleanup - effort S*

Both callers of `BuildTypedConfirm` re-test `typed == name` (src/Game/TypeEditorDialog.cpp:580, src/Game/WorldsDialog.cpp:136). So the rule lives in three places, counting the builder (src/Game/DialogLayout.cpp:237). The re-check is documented: Enter is not gated by `enabled` (DialogLayout.h:154).

Simplest fix: have the builder gate onSubmit on the match. That drops both re-checks and the redundant `m_deleteBtn->enabled` lines. A full shared component would buy little, because the two flows handle a refusal differently for real reasons.

### C92. Dead UI API: panel-rect AddCloseButton, CloseButtonRect, DialogTitleFont/DialogTextFont, TextOutput
*low - cleanup - effort S*

None of these has a caller in src/ or tools/:
- The panel-rect `AddCloseButton` and `CloseButtonRect` (src/UI/Controls.cpp:2471).
- `DialogTitleFont`/`DialogTextFont` (Controls.cpp:2495).
- `ui::TextOutput` (Controls.h:107). It allocates per line, which makes it a trap if revived.
- The free `ui::Rem(ctx, n)` (src/UI/Units.cpp:10).
- `FloatingPanel::Dragging`.
- `Skin::luma`, which is written (src/Game/GameUI_Stone.cpp:103) but never read.

All three close boxes already use the slot overload. Their slot sizes differ only because each sits in a different unit system.

Fix: delete these and fix the docs. CLAUDE.md still names the dead AddCloseButton and the font helpers, and ARCHITECTURE.md:42 lists TextOutput.

### C93. Settings is the one menu page not on a PageCard
*low - cleanup - effort S*

BuildSettings places a raw TabControl and a Back button that is not carved (src/Game/GameUI.cpp:410, :869). As a result, RenderMenuOverlay (:2400) and RenderPauseOverlay (:2443) each decide page by page whether to draw the floating title and subtitle.

Putting Settings on a card would remove those branches. One catch: `AddPageCard` (:1031) is hardwired to `m_savesUi`, while Settings lives in `m_settingsUi`, so AddPageCard must take a context first. BuildStoneTab (GameUI_Stone.cpp:245) could then reuse an exposed SettingsTab, keeping its own gap.

### C105. WorldsDialog keeps a console-only create path and some dead members
*low - cleanup - effort S*

`WorldsDialog::Create`/`onCreate` (src/Game/WorldsDialog.cpp:83) are reached only by the `worlds dialog create` console verb (src/Game/Game_DevWorld.cpp:467). That verb's comment says it reports what a mouse would get, which is no longer true. `m_newName` is only ever cleared.

- **Fix:** delete the create path. `Game::CreateWorld` already refuses a duplicate name, and newworlddialog.eval checks that on the real UI path. The finder's claim that the user's path lacks this check was wrong.
- **Eval:** drop worldsdialog.eval:17-19 and move its no-name case to `worlds newdialog create`.
- **Also dead:**
  - `NewWorldDialog::m_noteLabel`.
  - `m_device` in ValidateDialog and InspectPicker.
  - The `InstanceInspector::FormRow` shim (src/Game/InstanceInspector.h:132).

### C106. Two seed-reroll rules for one generator; Create & play skips PressCreate
*low - cleanup - effort S*

- **Two reroll rules:** the wizard rerolls with an LCG (src/Game/NewWorldDialog.cpp:229), while GenerateDialog uses `std::random_device` (src/Game/GenerateDialog.cpp:194). Put one rule in GenerateKnobs and use it in both.
- **Create & play:** this footer (GenerateDialog.cpp:243) re-implements `PressCreate(true)`. Have it call `PressCreate(true)` and then onPlay. The mode flip it would add does not matter, because PlayLevel closes the dialog.
- **Leave alone:** the wizard's simplified rows. They map onto derived knobs on purpose (src/Game/Game_Generate.cpp:321).

### C108. PartyCreationPage::RefreshFaces copies SyncPortraits and drops its warning
*low - cleanup - effort S*

RefreshFaces (src/Game/PartyCreationPage.cpp:415) runs the same steps as `Game::SyncPortraits` (src/Game/Game.cpp:765): compare the id, drain once, load portraits\<id>, re-point the portrait. The copy has lost the "missing portrait" warning, and it runs every frame.

It loads faces for preview Characters, so it cannot simply call SyncPortraits. CLAUDE.md's "ONE loader" line refers to the roster paths.

Fix: a shared slot helper of about 20 lines that keeps the warning, plus one `PortraitStem(id)` for the path, which is built in three places (also PortraitPicker.cpp:309).

### C110. PortraitGrid and StonePicker share a little grid code
*low - cleanup - effort S*

Both grids work out the column count from tile size and gap, and both mark the current tile with the same two DrawBorder calls (src/Game/PortraitPicker.cpp:138, src/Game/StonePicker.cpp:162). Otherwise they differ for real reasons: portraits virtualise 2879 entries, while stones are a captioned Len::Fit grid of 28.

AssetPicker's widget-per-tile grid has no real virtualisation gap, because ScrollArea already culls its tiles and ThumbCache loads only the visible ones. A small column helper and a current-mark helper are enough; a full shared ThumbGrid would not pay for itself.

### C112. PartyCreationPage::Tick reformats its labels every frame
*low - rule - effort S*

GameUI::UpdateMenu calls Tick every frame (src/Game/GameUI.cpp:2124). Tick (src/Game/PartyCreationPage.cpp:438) then reformats:
- the traits, pools and points labels,
- the five stat strings,
- `Refusal()`,
- and one string per face slot.

That is about 15-20 debug allocations a frame for text that changes only on an edit. It runs on the title screen, outside the allocation guard. Every edit already goes through `Touch()`/`Structural()`, which set `m_previewDirty`. So Tick could refresh only when that flag or the selection changes.

Against a documented decision: CLAUDE.md and PartyCreationPage.h:19 say "Tick rewrites the live text each frame".

### C113. PartyCreationPage hygiene: a fixed two-slot array, Stack downcasts, "human" in code
*low - cleanup - effort S*

- **The one real flaw:** SetPick (src/Game/PartyCreationPage.cpp:262) bounds-checks against `kSkillPicks`/`kItemPicks`, then writes a fixed `std::array<std::string, 2>` and uses `1 - slot`. Raising either constant to 3 (src/Game/PartyRules.h:39, 44) writes out of bounds. Size the array from the constants, or static_assert them.
- **Smaller:**
  - The Build* helpers downcast `Widget&` to `Stack&`.
  - NewMember hard-codes the race id "human" (:146).
  - The remove mark is a plain "x" (:123), not the carved cross used on stone.

### C114. Dead members and stale comments in the game pages
*low - cleanup - effort S*

- **Dead:**
  - `ItemDetailsDialog::m_device`.
  - `ItemDetailsDialog::Render`'s Theme parameter: the function reads `m_ui.GetTheme()`, though src/Game/GameUI_Items.cpp:196 passes `m_settings.theme`.
  - `PortraitPicker::Update`'s dt.
  - `StonePicker::Current()`.
- **Stale comments:**
  - PortraitPicker.h:5 says party creation does not exist yet.
  - StonePicker.h:2 calls the tab "Stone"; it is Material.
  - ItemDetailsDialog.h:73 says Esc closes the dialog in Update. Esc actually closes it through `GameUI::DismissPopup`.

Delete the dead items and correct the comments.

### C116. The asset picker keeps its thumbnails and preview loaded after it closes
*low - cleanup - effort S*

`AssetPicker::Close` only clears `m_open`. The tile thumbnails, any pending bake sources and the preview look all stay resident until the next Open. In practice that is the last pool browsed: about 90 SRV slots (roughly 9% of the heap) plus one rig's maps. It is editor-only, and the claim of 160 tiles was too high.

`PortraitPicker::Close` already calls `m_thumbs.Clear()`, so the two clients of the shared thumbnail cache follow different rules.

- src/Game/AssetPicker.h:70
- src/Game/AssetPicker.cpp:111
- src/Game/PortraitPicker.cpp:274

Fix: in Close, drain the GPU once, call `m_thumbs.Clear()` and run RefreshPreview's reset block.

### C117. The asset picker builds each tile's badge string on every frame
*low - cleanup - effort S*

`AssetTile::DrawSelf` builds a `std::string` badge (`ResBadge`, or "glb"/"gltf") for every visible tile on every frame. The comment in `ApplyFilter` says not to do exactly that. Fix: compute the badge once per AssetInfo in Open.

- src/Game/AssetPicker.cpp:218
- src/Game/AssetPicker.cpp:146

The verifier narrowed this. It is editor-only and outside the guarded frame, so it does not break the steady-state rule. AssetDialog's per-frame Validate is deliberate (its own comment says so), and CurvePlot states no allocation rule.

## 4. Structure as it has grown

### C118. Game is one flat 331-file library, so its inner layering is unchecked
*medium - structure - effort L*

The nine-library graph is one-way and holds. But Game is a single `add_library(Game STATIC` target with 191 .cpp and 140 .h files: 96.8k of the 116.5k lines in src (83%). Linking `PUBLIC Core Platform Assets Animation Graphics UI Audio` (src/Game/CMakeLists.txt:194) lets any Game file include anything.

The inner layering already holds by convention. No GameUI, CharacterSheet, PartyHud, dialog or inspector TU includes DungeonWorld.h, and the rules files are nearly pure. RollTest compiles 14 rules TUs and sits in the default build (root CMakeLists.txt:99), so a purity break already fails the build. Those TUs still compile twice, though, and nothing checks the HUD or editor side.

Splitting into libraries would not fix the 54-TU rebuild on a DungeonWorld.h edit. That cost comes from the header's size (see C119).

Fix, in steps:
- Make the pure rules TUs (Roll, Combat, Defense, Resource, DamageLedger, Curve, Power, Carve, Generate, LightProfile, Trail, MonsterAI...) a real `GameRules` target. Game, RollTest and ThreadStress link it.
- Move DevConsole into its own library below Game.
- Add an include lint, e.g. "no HUD, dialog or inspector TU includes DungeonWorld.h".

The original five-library plan does not work as written:
- DungeonWorld.h:32 needs GameSettings, which pulls in HUD and UI types.
- `m_harness` is read inside the simulation (DungeonWorld_Combat.cpp:1089). CLAUDE.md keeps all harness state as one world member on purpose.

### C119. DungeonWorld mixes the simulation, the editor back end and the renderer in one class behind a 4761-line header
*medium - structure - effort L*

`class DungeonWorld` runs from src/Game/DungeonWorld.h:79 to :4759. That is 73 nested types, about 150 data members and about 525 out-of-line methods across 26 .cpp files (21.3k lines). 35 files include the header directly and 54 TUs include it transitively. 116 of the 386 commits since 2026-09-01 touched it.

Comments make up 59% of the header, and it has section banners. Still, it carries the whole editor, census, validation and harness API. The class also keeps every level's stashes and the overworld undo (h:4555-4654), even though h:1530 says it is "the simulation of one level". ResetForNewGame/ResetForEval (DungeonWorld_Save.cpp:58-160) reset about 150 fields by hand.

Verifiers narrowed one claim: this does not block the brief's targets. fx::Deal/ITarget and ai::IWorldView already sit outside the class.

Fix, cheapest first:
- Lift the nested types other headers need (C120) and PoolModelLook (C121).
- Extract the light budget (plus stones and tracks) and an IconBaker as collaborators held by forward-declared pointer.
- Split the three oversized TUs (C122, C123).
- Last, extract the level store and editor history together, keeping whole-snapshot undo. C136 covers where the history should live.

Each piece gets its own Reset(). Keep the fx adapters nested. Moving the private kind structs saves no includes, because nothing outside the class uses them.

Against a documented decision: CLAUDE.md puts the remote-edit seam and the level stashes on DungeonWorld.

### C120. Headers include all of DungeonWorld.h to get one nested type each
*medium - structure - effort M*

A nested type cannot be forward-declared, so three headers pull in the 4761-line header for one piece each:
- src/Game/MapEditor.h:22, for MoveTarget. It then forward-declares `class DungeonWorld;` at :44 anyway.
- src/Game/MapView.h:32, for LevelBrowse. Its inline methods at :197 and :202 also call `m_world->BrowseLevel`.
- src/Game/AssetPicker.h:31, for PoolModelLook and kIconSize.

MonsterKind, Monster, ItemKind, Door and Fire (h:2205 onward) are plain data but private, with no friends. That means an extracted collaborator could not hold them.

Fix: move MoveTarget and LevelBrowse into small headers, and move MapView's two inline bodies into its .cpp. Move PoolModelLook out (C121). Then forward-declare DungeonWorld in all three headers. Game.h already holds a `std::unique_ptr<DungeonWorld>`, so it can forward-declare too. Move the kind structs into their own headers when an extraction needs them.

### C121. The asset picker's tile-framing code is parked on DungeonWorld, and two previews pick a rig's idle by different rules
*medium - structure - effort M*

PoolModelLook is about 175 lines of picker framing: Slender, Well, ViewTilt, FrameWallFixture and the tilt constants. It sits at src/Game/DungeonWorld.h:226-399. The static LoadPoolModelLook (h:405, src/Game/DungeonWorld_Models.cpp:150) and its mesh surgery (Models.cpp:30-441) use no world state. Their only users are AssetPicker and Game_Wiring.

Two rules have drifted apart:
- **Idle clip.** The picker uses `anim_idle`, else an `idle__*` clip, and never the first clip (Models.cpp:170-186). MonsterPreviewFor falls back to `clips.front()` (DungeonWorld_Load.cpp:862-866). For the kit that clip is a spawn lying on the floor. This only shows when no idle is authored.
- **Material choice.** The picker approximates the world's per-kind rule in one line (Models.cpp:220).

Fix: move PoolModelLook, the loader, ClipBelow and DropWellSkin into Game/PoolModelLook.{h,cpp}. Put MultiMaterialModel, BuildMultiMaterialModel, ApplyPbr, BuildLiquid and ClearGlassForLiquid in a shared model-material TU. Share one `PickIdleClip`.

### C122. DungeonWorld.cpp and DungeonWorld_Load.cpp are grab-bags; item pick-up and drop live in the Load file
*medium - structure - effort M*

Both files are over the ~2000-line rule (2355 and 2343 lines).

src/Game/DungeonWorld_Load.cpp also holds:
- editor previews and details (834-1047) and monster config (763-832)
- liquid building
- the guarded-frame click path TryPickItem/DropItemAt (:1541, :1613)
- BuildMultiMaterialModel (:1824), whose callers are all in _Models.cpp
- BuildFires and turbidity (2152-2287), although _Fires.cpp exists
- an orphaned DecorationKindFor comment at 1816-1819

Keeping click code beside load code makes it easy to add an allocation to a guarded frame by mistake.

src/Game/DungeonWorld.cpp holds UpdateMonsters (1094-1403), about 385 lines of inspector getters and setters (1492-1877), and formation, the AI snapshot and threat (1877-2355).

Fix: split by concern:
- pick/drop into _Items.cpp or _Throw.cpp
- previews into _Previews.cpp
- fires into _Fires.cpp
- monsters into _Monsters.cpp
- inspector accessors into _InstanceEdit.cpp

### C123. DungeonWorld_Combat.cpp is 2921 lines covering about a dozen concerns, including the monster intent executors
*medium - structure - effort M*

The file's banner (src/Game/DungeonWorld_Combat.cpp:1-6) claims only combat. It also holds:
- skills and XP, rest, supplies and ConsumeItem (27-512)
- growth and pace
- burn presentation
- monster stepping and track recording
- the cast facade, breakables and blasts (2336-2921)

That is well past the ~2000-line rule.

A verifier corrected one part. UpdateKiter, UpdateFleer, UpdateReturner and UpdatePatroller (1637-1722) execute *intents*. They are dispatched on `monster.intent.mode` (src/Game/DungeonWorld.cpp:1317-1334), not on archetype. The archetype is chosen separately in ai::Brain::Think (src/Game/MonsterAI.cpp:122-158).

Fix: split into _Growth, _Supplies, _Targets, _PartyAttack, _Cast, _Breakables, _Blasts and _MonsterActs. For "one class per behaviour", build two halves: archetype strategy objects in the Brain, and executors keyed by intent on the host side.

### C124. Game is a god class: dialogs, editor services, the world tier, save mapping and icons
*medium - structure - effort L*

Game.h is 1305 lines, with 47 includes (including DungeonWorld.h) and about 150 data members. The implementation is 14,649 lines across 17 TUs. Game.cpp alone is 3003.

Game.h holds:
- 20 dialog, inspector and picker members (src/Game/Game.h:1121-1173)
- the preview animator state (:1199-1208)
- the overworld (:977-988)

Game.cpp holds:
- about 240 lines of dialog wiring in the constructor (src/Game/Game.cpp:167-409), although Game_Wiring.cpp's banner claims that job
- field-by-field CharState mapping in SaveGame/LoadGame (1140-1476)
- an icon library (813-995)

Other loose members:
- The bake job is seven loose fields (Game.h:1234-1246), stepped from four TUs.
- `"rune_tablet"` is named only in src/Game/Game_Wiring.cpp:661.

The comment at Game.h:4 ("thin coordinator") is stale, and so is CLAUDE.md's "Game.cpp is just the app state machine + wiring".

Verifiers narrowed one claim: this does not block a shared dialog base. The 7 inspectors already share InstanceInspector as Game members (src/Game/Game_Inspect.cpp:16). The real symptom is the hand-written IsOpen chains in Update (Game.cpp:2189-2400) and Render (2906-2963).

Fix:
- Add a shared dialog interface and one ordered list.
- Then extract an EditorHost that owns the dialogs, previews, a BakeJob and OpenInspectorFor.
- Move the CharState mapping into SaveGame.cpp.
- Extract the content operations and the overworld when those areas are next touched.

### C125. Game.cpp's 735-line UpdateStates copies the world-tick tail four times, and the copies have diverged
*medium - structure - effort M*

UpdateStates (src/Game/Game.cpp:1911-2646) repeats "update the world, follow a transition, refresh the HUD" in four places. The four copies now behave differently:
- **Console (2036-2042):** it only ticks in `Playing`. With the console open over a level, the character sheet freezes the world, which breaks "the sheet is not a pause". It ignores the editor's pause, so the console over a paused editor resumes the simulation. It never consumes a transition, so a transition raised there stays latched while the old level keeps running. It never calls SetResting.
- **Map overlay (2505-2520):** it inlines FollowLevelTransition (:1491) without SetResting. It also declares a function-local `static const Input kNoInput` that shadows the namespace-scope one at :81.
- **Sheet (2165) and Playing (2633):** these run the full sequence.

Fix: add one `TickWorld(input, wdt, acceptInput)` that covers update, transition and HUD/rest, and call it from all four places. Keep a single kNoInput. Split UpdateStates and move it, Render and the loading tasks into their own files, which also gets Game.cpp under 2000 lines.

### C126. GameUI.cpp has grown back past the ~2000-line rule, and eight other files are over it too
*medium - structure - effort M*

src/Game/GameUI.cpp is 2484 lines, although GameUI_Items.cpp:5-6 records an earlier split made for this same rule. Its biggest functions:
- BuildSettings (404-874) builds five tabs inline. Its `section` lambda (829-834) is open-coded twice earlier (483-486, 527-530).
- BuildHud runs 1534-1891.
- The file also holds video staging, menus, the sheet build and rendering.

The other files over 2000 lines are DungeonWorld.h 4761, Game.cpp 3003, DungeonWorld_Combat.cpp 2921, ModelBaker.cpp 2677, RollTest/Main.cpp 2549, Controls.cpp 2508, DungeonWorld.cpp 2355 and DungeonWorld_Load.cpp 2343. Most are covered by their own entries.

Fix: create GameUI_Settings.cpp (one BuildXTab per tab, a shared Section helper, video staging) and GameUI_Hud.cpp. Optionally add GameUI_Menus.cpp and GameUI_Render.cpp.

### C127. Controls.cpp is 2508 lines, and DropDown copies ScrollArea's scrollbar against the one-owner rule
*medium - structure - effort M*

DropDown has its own scrollbar (src/UI/Controls.cpp:793-899, 1120-1126): track, thumb, drag and draw. It matches ScrollArea's (2213-2319) line for line, down to the `Rem(0.9f)` thumb minimum. That breaks CLAUDE.md's "ScrollArea owns ALL scroll/thumb/clip behaviour". docs/ui-hierarchy.md:257 called SlotList's copy "the fourth and last", so this one is a regression. The wheel steps already differ (`RowH()` against `Rem(1.75)`), though that may be deliberate.

The popup draws in the overlay pass, so it cannot host a child ScrollArea. Factor the bar into one non-widget `ui::ScrollBar` that both use.

Also, the ink solver (DrawCarvedText, Legible, ResolveInks, ContrastRatio, at 2045-2135) sits between MenuList::UpdateSelf and MenuList::DrawSelf. It writes only Skin fields, so it belongs in Skin.cpp. Then split the rest of the file by widget family.

### C131. Graphics' public types and shaders are named after game features
*medium - structure - effort M*

ARCHITECTURE.md rule 3 says engine modules know nothing about dungeons, parties or items. Graphics' API names them anyway:
- `BarKind {Health, Stamina, Mana, Effort, Food, Water, Progress}` (src/Graphics/SpriteBatch.h:45)
- `sightCell/sightTint/sightHole` "the Sight spell" (src/Graphics/Lights.h:94-106), cut in assets/shaders/scene.hlsl:549-579
- `emissiveGroove` for "a rune tablet" (src/Graphics/Renderer.h:71-75), with RuneBaker's 0.45 carve scale hard-coded in scene.hlsl:534-536

Nothing documents an exception. The include graph is clean: Game fills these fields.

One verifier rated this low and suggested fixing it only when the code is next touched. Renaming leaves the per-feature shader code in place, and BarKind cannot simply move to UI, because bar.hlsl switches on the numbers.

Fix: name the mechanisms (a generic cutaway volume, an emissive mask whose scale is passed in, an opaque `u32` bar style), or amend rule 3. At minimum, pass in the 0.45 constant rather than mirroring it by hand.

### C136. Undo and stair moves reach Game's WorldMap and Project through raw pointers, writing a project held as const
*medium - structure - effort M*

DungeonWorld.h:458-466 says the borrowed WorldMap is for undo alone ("NOTHING ELSE HERE READS IT"). RemapArrivals (src/Game/DungeonWorld_Move.cpp:433-466) reads it and mutates it through MutableLocation.

src/Game/Game.cpp:547 also lends pointers to `m_project.startLevel/X/Z`. Move.cpp:461 and DungeonWorld_Undo.cpp:57 write through them, while the world holds `const Project& m_project` (h:3966).

The arrival rule also has a second copy, Game::ArrivalsOn (src/Game/Game_Generate.cpp:600-619), and that copy is the buggier one:
- It ignores the opening when `startLevel` is empty.
- It splits a dungeon's level list with ParseTags, which lowercases. Project.cpp:306 notes that this is wrong for filenames.

Its only caller is regenerating a level. Rerolling a level with a capitalised stem therefore ignores world-map doorways, and can leave the party arriving inside rock.

Fix: one shared arrival rule. Move the undo history into an editor-level history in Game, with each step holding the WorldMap, the opening and DungeonWorld::CaptureLevels(). MoveObject/ResizeLevel return a square remap that Game applies.

Against a documented decision: CLAUDE.md and h:458-466 lend the world "for the undo history ALONE". RemapArrivals is the second use that the comment itself names as the moment to move the world.

### C138. Light state (budget, fades, profiles) lives on DungeonWorld; extract a LightSystem
*medium - structure - effort L*

About 110 lines of light state sit directly on DungeonWorld (src/Game/DungeonWorld.h:3975-4082): profiles, candidates, origins, fades, the reach map and stress lights. Six TUs touch it: DungeonWorld.cpp, _Light, _LightBudget, _SpellLight, _Flight and _Render. This bloats the 4761-line header, and it keeps light selection out of RollTest. Only LightTiles and LightProfile are pure today.

Fix: extract a `LightSystem` with Begin/Push/Select/Describe, following the ShadowScheduler pattern (src/Game/ShadowScheduler.h:36). It owns profiles, candidates, fades, reach, stress lights and the scheduler. It needs the map, camera, budget and the party's square.

Minor: SelectLights scans the 320-entry fade table twice per candidate (src/Game/DungeonWorld_LightBudget.cpp:143, :174). Carry the index in LightCandidate instead.

The verifier narrowed one claim: the two reach searches are not copies. Only their neighbour tables match, so sharing a BFS would save little.

### C140. What happens to the item slot on eat, memorize or throw is decided only in GameUI
*medium - structure - effort M*

The world already does the arithmetic. DungeonWorld::ConsumeItem (src/Game/DungeonWorld_Combat.cpp:359) handles meters, potion restores, cures and the downed refusal. But only GameUI decides what happens to the slot:
- EatSlot keeps the item when nothing was restored, and otherwise steps it to its leftover id (src/Game/GameUI_Items.cpp:630-660).
- MemorizeSlot learns the rune and clears the tablet (:594-606).
- ExecuteUse clears the hand after a throw (:489-498).

The `consume` dev command (src/Game/Game_DevParty.cpp:1327) calls ConsumeItem directly, on purpose. So no script reaches the step-down or the keep-on-no-effect rule. Memorize is covered through `itemdetails memorize`.

Fix: add a world-side `UseSlot(member, ItemSlot&, verb)` that returns a result for the UI to narrate, and point `consume` at it. SelectUse, UseValidFor and DefaultUseFor are per-hand UI bindings and can stay in GameUI.

Against a documented decision: CLAUDE.md names GameUI::EatSlot and MemorizeSlot. That records where the code sits, not a rule, and the move keeps them allocation-free.

### C143. Dev command registration: giant functions, copied preambles, and a separate no-world list
*medium - structure - effort M*

The three Register functions are each one function of lambdas:
- RegisterDevCommands: src/Game/Game_DevCommands.cpp:37-1504.
- RegisterPartyCommands: Game_DevParty.cpp:30-1607.
- RegisterWorldCommands: Game_DevWorld.cpp:32-1234.

The `editor` lambda (Game_DevCommands.cpp:246-663) is an if-chain of about 20 verbs. It repeats the open-or-switch-to-Editor preamble 9 times and the "no palette row" arm block 3 times (493/536/573). Game_DevParty parses the member argument about 21 times, each a little differently.

Which commands may run with no world is a hand-kept name list, `kNoWorldNeeded` (src/Game/Game.cpp:471-478), kept apart from CmdInfo (src/Game/DevConsole.h:82-87). `inputpoke` is missing from it, so the title screen refuses it for no reason. Drift fails closed.

Fix:
- Add `needsWorld` to CmdInfo.
- Add `devargs::MemberArg`.
- Move the editor verbs to Game_DevEditor.cpp behind a verb table whose miss calls RefuseUsage, with EnsureEditorMode/CloseInspectors helpers.

### C144. GameUI talks to the world through about 45 std::function members, one with a dangling-reference trap
*medium - structure - effort M*

GameUI.h has about 45 callback members. Many are world queries, not actions: itemCommands, spellDefs, defenseFor/With, itemDetails, itemPreview, torchActFor, consumeLeaves, exertMax, partyLeader and moveCounter. Each query takes three hops:
1. The wiring lambda null-checks m_world (src/Game/Game_Wiring.cpp:192-283).
2. GameUI null-checks the function (src/Game/GameUI.cpp:1219).
3. The sheet and the cards hold a third std::function.

The header documents a trap (src/Game/GameUI.h:430-434): the itemCommands wiring lambda must spell out its reference return type, or it returns a copy and the reference dangles.

Fix: an `IItemServices` interface, implemented by an adapter over DungeonWorld. That gives compile-checked return types and no null checks. Keep the on* callbacks for real state-machine actions.

Against a documented decision: CLAUDE.md routes UI-to-state-machine ACTIONS through on* callbacks. These are queries, which the codebase elsewhere models as narrow interfaces (ai::IWorldView, fx::ITarget).

### C151. MapView has become the editor's shell
*medium - structure - effort L*

MapView::Render runs src/Game/MapView.cpp:890-1876 (about 986 lines), and Update runs 496-888. MapEditor.h:4-6 says the split keeps the shared viewport small. In practice the toolbar, tool strip, rectangle/shape drag state, edge-resize drag, overview dock, issue overlay and status bar all live in MapView, about 3.5k lines in total.

The two classes reach into each other:
- MapView_Tools.cpp:126 and 217-252 write MapEditor's public `preview` (src/Game/MapEditor.h:224).
- MapEditor holds a `MapView&` back-reference, used 35 times.

A player-map change means reading a 1000-line function full of editor branches.

Fix: split Render into per-layer methods. Then move the editor chrome and gesture state into an editor-shell object that composes the viewport. At minimum, make `preview` private and keep the drag state beside it.

Against a documented decision: the hand-rolled dropdown (src/Game/MapView.h:582) and dock scroll (src/Game/MapView_Docks.cpp:392) break CLAUDE.md's "ui::ScrollArea owns ALL scroll" rule. That rule is waived by MapView's out-of-tree status, which docs/ui-hierarchy.md records. See C152.

### C152. The editor palette re-implements text field, checkbox, scroll and tooltip
*medium - structure - effort L*

The palette hand-rolls the controls the UI library already has:
- A filter text field with a 24-character cap, repeated in SetFilter (src/Game/MapEditor_Categories.cpp:192), plus a hand-drawn caret (src/Game/MapEditor.cpp:1339).
- The Catalogue checkbox (:1363-1373).
- A wheel scroll with a magic 28.0f step and no thumb (:594). The right dock copies it (src/Game/MapView_Docks.cpp:393).
- Its own row tooltip.

BuildPaletteRows is rebuilt in five functions, once per hit-test or draw. Each new palette control means another hand-made widget and another hit-test, and `uioverlap` audits none of it.

Fix: host the left dock body in a UIContext with a ScrollArea over a Repeater, a TextField and a Checkbox, and let the right dock share the ScrollArea. Interim step: lift the scroll step and the filter cap into one place.

Against a documented decision: docs/ui-hierarchy.md "Out of scope" exempts MapView/MapEditor. The palette has since grown enough controls that the exemption costs more than converting it.

### C128. Rules-side files pull in UI and D3D12 headers
*low - structure - effort S*

Three include chains drag UI or D3D12 headers into rules-side code:
- Character.cpp:5 includes GameSettings.h only for kDefaultMemberColors (:131). That pulls in UI/Controls.h, UIContext.h, Party.h and PartyHudTypes.h.
- GameSettings.h:18 includes PartyHudTypes.h (and with it Character.h and the item banks) only for HudPanelLook.
- src/Game/Projectiles.h:35 includes ParticleBatch.h for one method, so every Spell TU sees the D3D12 headers.

Fix: give the colours, HudPanelLook and ui::Theme small homes of their own, and make billboard building a free function.

The verifier calls this include hygiene, not a layering breach. Party.h's Platform/Input.h include is not an upward edge.

### C129. DungeonWorld includes the HUD draw header for one colour lookup
*low - structure - effort S*

src/Game/DungeonWorld_Render.cpp:9 includes PartyHudDraw.h, and through it UI/Controls.h, only for RuneGlowColor at :1034. Fix: move RuneGlowColor and the groove constants beside Spells.h's ElementColor.

DungeonWorld also holds `const GameSettings&` (src/Game/DungeonWorld.h:3965), but it reads only the mesh/texture suffixes, maxPointLights and a log label. A small RenderQuality value would do. GameSettings.h's own comment documents that reference, so that half is optional.

### C130. DevConsole is one class mixing the command shell and the perf dashboards
*low - structure - effort M*

DevConsole is 9 files and 4,465 lines. It includes only Core, Graphics, Platform and UI headers. Its only game knowledge is the CmdGroup enum (src/Game/DevConsole.h:53) and its titles. One class holds two things:
- The command shell the eval runner uses (RunLine/ConsumeRefusal).
- The perf, profile, health and threads panels.

src/Game/Game.h:47 includes it, which reaches 18 TUs, and the panels sample every frame, headless runs included (src/Game/DevConsole_Perf.cpp:24).

Fix: split a CommandConsole from a ReadoutPanel. Moving it to its own library is optional.

The verifier rates this organisational: there is no second consumer, and the per-frame cost is small.

Against a documented decision: DevConsole.h:19, "One class, seven files".

### C132. The texture-file loaders sit in Game/AssetUtil
*low - structure - effort M*

TryLoadTextureFile and LoadTextureThumb (src/Game/AssetUtil.cpp:44, :61) are the engine's only file-to-GPU texture path. They use only Assets and Graphics, and Graphics already links Assets, so moving them into gfx is cheap.

The verifier narrowed the rest of the claim:
- The shared icon globals (:90-100) are the documented close-icon convention (CloseIcon plus ReleaseSharedIcons in ~Game).
- The character-sheet icon trap was a load-order bug, already fixed.
- A UI-owned cache would still have to be released before the device.

So only the loader move is worth doing.

### C133. Catalog/Serialize live in Game, so FontLibrary cannot read fonts.cat
*low - structure - effort M*

Serialize and the Catalog container (src/Game/Catalog.h:113) depend only on Core and Assets/File, yet they live in Game. So FontLibrary cannot parse its own config (src/UI/FontLibrary.h:24), and Game builds it instead (src/Game/Game.cpp:55-73). Fix: move Serialize and the format-level part of Catalog into Assets.

The verifier narrowed this: FontLibrary is the only engine module working around it. The RollTest comment cited as evidence actually endorses the wall, and moving Catalog would not let RollTest link Balance.

### C134. Small engine-generic helpers parked in Game
*low - structure - effort S*

Several helpers in Game do not depend on Game:
- DialogLayout.h includes only Graphics and UI (its one Game dependency is ToolbarIcon).
- ThumbCache.h includes only Core and GraphicsDevice.
- LoadQueue.h includes only Core.

The HUD and the editor both use DialogLayout and ThumbCache. There are also two identical Vec4 `Mix` copies (src/Game/GuardSlider.cpp:39, src/UI/Controls.cpp:2060).

Fix: move DialogLayout and ThumbCache to UI after C132, move LoadQueue to Core, and add a Vec4 Lerp to Core/MathTypes. GuardSlider, CurvePlot, MessageLog, MenuPanel and HudTray rightly stay in Game.

### C135. Main does more than glue, and the command line is parsed four times
*low - structure - effort S*

The command line is walked four times with `CommandLineToArgvW(GetCommandLineW())`: twice in Main (`-headless`, `-eval`) and twice inside Game (`CommandLineHas` for `-project`, and `ChooseProjectFolder`, which checks `-eval` again). The two layers agree today, so this is cleanup, not a bug. Main also carries its own walk of the profiler tree under a stale comment saying it stands in "until that exists". The console panel it refers to now exists.

- src/Main/Main.cpp:79, :123
- src/Game/Game.cpp:37, :106
- src/Main/Main.cpp:288 (stale comment), next to src/Game/DevConsole_Profile.cpp:40

Fix: parse argv once in Main into a `LaunchArgs` struct and pass it to Game's constructor. Delete the stale comment. The two tree walks produce different output (a full log dump vs. level-gated rows), so merging them needs one shared walker, not a straight swap.

### C137. Party rules (tick, rest, supplies, leader, throw) live inside DungeonWorld
*low - structure - effort L*

TickParty, rest, supplies, skill XP, leader, the wipe latch and throw cooldowns all live in DungeonWorld (a header of about 4,760 lines), and many are public only so Game can call them. A verifier narrowed this. DungeonWorld is the per-world object, not a per-level one, so a world-map journey (src/Game/Game_World.cpp:466) does not depend on a loaded level. What remains is a factoring preference with no defect behind it. If a `PartyRules` unit is ever pulled out, it should keep the shared RNG.

Against a documented decision: src/Game/DungeonWorld.h:592-596 makes the RNG "Deliberately not a second generator" so that `seed` covers the whole run.

### C139. The Character-to-save mapping lives in Game::SaveGame/LoadGame
*low - structure - effort S*

About 210 lines map Character fields to `SaveData::CharState` and back: about 77 out at src/Game/Game.cpp:1174-1250 and about 133 back at src/Game/Game.cpp:1289-1421. The return trip includes the v17 back-solve, the v25 supply defaults and the portrait check. The finder's ~300 was an overcount. This is a placement problem, not duplication or a bug: a new member field means two edits far apart inside the state machine's file.

Fix: move the code into the existing SaveGame.cpp as `CaptureCharacter` / `RestoreCharacter`, side by side. Pass in Balance, the EffectBook and the portrait catalog.

### C141. The Game constructor still holds about 31 dialog-wiring lambdas despite the "split out" banner
*low - structure - effort S*

The banner at src/Game/Game.cpp:505-508 says the wiring was split out of the constructor. In fact about 31 map-view, balance, level-settings, validate and generate callbacks are still wired in the constructor body (src/Game/Game.cpp:171-406). Balance's onApply and onSave also repeat the same three lines (src/Game/Game.cpp:203-211).

Fix: move that wiring into named `Wire*` functions beside the others, share the Balance apply body, and correct the banner. A verifier dropped the crash claim. The null-world checks in src/Game/Game_Wiring.cpp follow the reachability rule written at :188-191. It is just not enforced anywhere. A `PartyActions` interface is optional taste. CLAUDE.md's "wired in the Game constructor" still holds, because the Wire* functions are called from it.

### C142. Eval harness state in Game.h: an orphaned comment, a stale one, and a latch
*low - structure - effort S*

The finders asked to group Game's harness fields the way `DungeonWorld::m_harness` groups the world's. A verifier narrowed this:

- The eval fields already sit in one block (src/Game/Game.h:697-720), and only src/Game/Game_Eval.cpp uses them. Folding them into an `EvalRun` struct or runner class is easy and would make a reset one assignment.
- Alloctest, the pokes and the governor are dev-console tools, which CLAUDE.md says are not harness machinery.

The concrete defects:

- `m_harnessOpensInLevel` (src/Game/Game.h:993) has its comment 128 lines away, at :865-871.
- It also sits under a stale roster comment that says "never resized". ResetRoster now resizes the roster (src/Game/Game.cpp:1008).

The flag is set once at src/Game/Game_Eval.cpp:59 and never cleared. That only matters inside harness runs: where a new game opens depends on whether a cold `reset` came first. Do not make it one-shot, because partycreation.eval relies on the latch. Make "this is a harness run" an explicit property of the runner instead.

### C145. The seven UIContexts are listed by hand in five places
*low - cleanup - effort S*

ApplyTheme (src/Game/GameUI.cpp:259), ApplySkin (:270), UpdateFonts (:2053), UiTree and UiTreeNames (:1513) each list the seven contexts by hand. UpdateFonts' own comment says `m_savesUi` was once left out. OpenConfirm (:1448) also re-applies a theme that ApplyTheme already set.

Fix: build one table of contexts in the constructor and loop over it everywhere. Delete the extra `SetTheme`.

### C146. Game_DevParty.cpp has drifted from its concern, and two comments sit on the wrong command
*low - cleanup - effort S*

The file header names 23 of the 32 commands the file registers. uimaterial and hudpanel are `CmdGroup::Settings`, and they and hudbars do not belong in a party file. Two rationale comments are attached to the wrong command:

- The `sheet` rationale (src/Game/Game_DevParty.cpp:848-852) sits above `uimaterial`, while `sheet` (:944) has none.
- The `seed` rationale (src/Game/Game_DevEval.cpp:49-52) sits above `logecho`, while `seed` registers at :408.

Fix: move both comments back, update the header, and move the three HUD/settings commands out. At 1,609 lines the file is still under the split threshold.

### C147. DrawProfileSection is one 895-line function
*low - cleanup - effort M*

src/Game/DevConsole_Profile.cpp:533-1427 is a single function with about ten local lambdas. It draws the header and verdict, the graph view (:683), the list view (:902) and two tooltips (:1281), with layout, height bookkeeping and hit areas mixed together in one scope.

Fix: split it into `DrawProfileHeader`, `DrawProfileGraphs`, `DrawProfileList` and `DrawProfileTooltips`, each taking the PanelCtx and a frame budget computed once.

### C148. The FPS meter is owned by the dev console
*low - cleanup - effort S*

`PerfMonitor` is a DevConsole member (src/Game/DevConsole.h:214), the console's constructor starts its OS sampler thread (src/Game/DevConsole.cpp:94), and the governor reads `m_console.Fps()` (src/Game/Game.cpp:1705). A verifier narrowed this to ownership tidiness, not a layering fault: the governor is an opt-in console feature, and CLAUDE.md keeps the console compiled into every build.

Fix: Game owns the PerfMonitor beside `m_threads` and lends it to the console.

### C149. WorldMap.h is a grab-bag with unused and backdoor API
*low - structure - effort S*

WorldMap.h holds the lever `FlagOp` (src/Game/WorldMap.h:52-63), the save's `WorldState` and the overworld map. As a result ButtonInspector.h:16, DungeonWorld.h:29 and SaveGame.h:24 include the whole overworld header. `WorldState::Flag` (:126) has no callers. The only caller of `MutableAreas()` (:287), src/Game/Game_DevWorld.cpp:982, just reads the list.

Fix: move FlagOp and WorldState into their own header, delete `Flag`, and switch that caller to `Areas()`.

### C150. DungeonWorld_Validate.cpp hides level install under a "validation" banner
*low - structure - effort S*

The banner (src/Game/DungeonWorld_Validate.cpp:1-13) says the file only gathers data for the checker. It also holds the generator helpers (:22-61), the in-place level replace `InstallLevel*` and its transient resets (:63-153), and `ReadOnlyLevelOf` (:155-172). `Validate()` starts at :174. The level-install code is bug-prone, and this is not where anyone would look for it.

Fix:

- Move `InstallLevel*` beside `BeginLevelLoad` in DungeonWorld_LevelIO.cpp.
- Move `ReadOnlyLevelOf` beside the stash helpers in DungeonWorld_Remote.cpp.
- Move the generator helpers beside the generator code.

### C153. Win32 key codes leak because Platform's `vk` table has only six keys
*low - cleanup - effort S*

src/Platform/Input.h:25 defines only six key names (Back, Return, Escape, Space, Up, Down). Code outside Platform therefore uses three other conventions:

- UI defines its own `kVkControl = 0x11` (src/UI/FloatingPanel.cpp:18).
- MapView tests raw `0x10/0x11/0x12` (src/Game/MapView_Tools.cpp:133-135, :215; src/Game/MapView.cpp:534).
- About 50 Game lines in 20 files use `VK_*`, reached through d3d12.h.

A verifier noted that the d3d12.h route is a legitimate dependency. The real problem is the mixed conventions and the unexplained hex in the modifier checks.

Fix: add Shift, Control, Menu, Tab, Prior, Next, Oem3 and so on to `vk`, then replace the hex values and `kVkControl`.

## 5. The graphics pipeline

### C163. SpriteBatch's fixed 4 MB arena aborts the editor map on a 128x128 level
*high - bug - effort M*

The game has one SpriteBatch, and it uploads into a fixed 4 MB arena each frame. Running out trips a DN_ASSERT, which aborts in every build. A quad costs 6 x 56 B, so one frame holds about 12,500 quads.

MapView::Render draws one quad per cell with no culling, and in Editor mode every cell is visible. The generator allows 128x128 levels. That is 16,384 cell quads (5.5 MB) before any text, so generating at the knob maximum and landing in the editor at fit zoom aborts. Anything above about 105x105 does the same.

- src/Graphics/UploadAllocator.cpp:18 (the assert)
- src/Graphics/SpriteBatch.cpp:126 (the 4 MB arena)
- src/Game/MapView.cpp:1036 (the cell loop)
- src/Game/GenerateKnobs.cpp:25 (the 128 limit)

Fix:
- Size the arena for the largest map (about 8 MB), or merge same-ink runs, or cache the cell layer in a render target keyed on EditRevision.
- On overflow, drop and count like the glass queue does.
- Add a high-water gauge beside the SRV gauge.

Culling to the screen does not help at fit zoom.

The verifier cut the ParticleBatch half of this finding to a low coupling gap. It is not reachable in practice.

### C178. Shadow-cube cache misses doors, smashed props and vanished corpses
*high - bug - effort M*

A cached cube re-renders only in four cases: a light change, a map Revision bump, a monster animating in range, or a move or flicker. The held torch has no `wander`, so its slot-0 cube is reused for as long as the party stands still. Several things the shadow pass draws never invalidate it:

- **Doors.** Open a door with no monster near, and the space beyond stays in the closed leaf's shadow until the party moves. Door toggles do not bump Revision.
- **Corpses.** When a death clip ends, the monster leaves the draw and AnimatedCasterNear on the same frame, so its shadow lingers. A blob with no skeleton does the same on death.
- **Smashed decorations and doors** leave their shadow behind.

Where: src/Game/ShadowScheduler.cpp:145, src/Game/DungeonWorld_Render.cpp:100, src/Game/DungeonWorld.cpp:442, src/Game/DungeonWorld_Combat.cpp:2357.

Fix: extend AnimatedCasterNear to count moving doors and cargo in flight. Add a fixed-size, allocation-free list of "caster changed this frame" points, tested against each light's sphere. A global revision would re-render all 8 cubes for any door anywhere. Add a check that opening a door re-renders slot 0.

The verifier notes that sconce and stone changes usually re-slot lights anyway.

### C194. Alt+Tab out of exclusive fullscreen skips ResizeBuffers, so the next Present aborts
*high - bug - effort M*

The code's own comment says a flip-model swapchain needs ResizeBuffers after every fullscreen/windowed switch (src/Graphics/GraphicsDevice.cpp:223). Only SetFullscreen enforces it.

The failure chain:
- MakeWindowAssociation passes only NO_ALT_ENTER (:109), so DXGI still drops exclusive mode by itself when focus is lost.
- Nothing notices: there is no WM_ACTIVATEAPP handling, no occlusion handling and no per-frame GetFullscreenState.
- SetFullscreen sizes the window to the mode before entering (:265-271), so the restored window has the same size.
- WM_SIZE is therefore filtered (src/Platform/Window.cpp:117) and Resize early-outs (:213).
- The DN_HR'd Present (:531) then aborts.

Trigger: any Exclusive session (including a boot with fullscreen=2) followed by Alt+Tab, the Windows key, or a toast taking focus. If the game survives, it stays windowed while settings.ini still says Exclusive.

Fix:
- Have GraphicsDevice remember the last fullscreen state, compare it at the top of BeginFrame, and call RecreateSwapChainBuffers when it changes.
- Skip Present while minimized.
- Re-enter Exclusive on WM_ACTIVATEAPP through SetFullscreen.
- A `display drop` dev command would let a harness test this.

The verifier could not run DXGI, so the exact failing frame is inferred.

### C154. Quality hot-swap reloads only surface textures
*medium - bug - effort M*

ApplyQuality reloads only the wall, floor and ceiling sets (src/Game/DungeonWorld_Load.cpp:2332). LoadPropTextures caches by set name alone (:1730) and the cache is never cleared. So props, doors, fixtures, monsters and runes keep whatever tier they first loaded at:

- Going from Low to Ultra leaves 1k doors beside 4k walls.
- Going from Ultra to Low keeps the props' 4k VRAM, which defeats the point of lowering quality.

Fix: key the cache by set plus resolution, and rebuild each entry in place on a swap (kinds hold stable pointers into it). Also correct docs/ARCHITECTURE.md:114, which claims a swap reloads every texture.

### C158. Thumbnails and swatches draw too dark
*medium - bug - effort S*

SpriteBatch writes its texture sample straight into the UNORM back buffer (assets/shaders/sprite.hlsl:72), so anything it draws must be loaded linear. LoadTextureThumb defaults to sRGB (src/Game/AssetUtil.h:53). Two kinds of image take that default:

- The asset picker's texture tiles (src/Game/AssetPicker.cpp:263).
- The editor's swatches (src/Game/DungeonWorld_Editing.cpp:139, :160), which also borrow the world's sRGB albedo.

Both draw much darker than the material looks in the world. The placeholder icon (src/Game/Game.cpp:847) is darkened twice.

Fix: make LoadTextureThumb default to linear, give swatches their own linear thumbs, and document the rule. The portrait picker already follows it.

### C179. Draw parts are rebuilt separately for world, icons and previews, and have drifted
*medium - bug - effort L*

The world draw, the map-icon bake and the inspector previews each assemble a kind's meshes and materials by hand. The copies have drifted:

- **Brazier icon:** the map icon has no coal bed (src/Game/DungeonWorld_Render.cpp:1177).
- **Door and lever previews:** the door preview shows only frame and panel, without trim or opener (src/Game/DungeonWorld_Doors.cpp:446). The lever preview has no plate (:762).
- **Door and lever draws:** they ignore `multimaterial` and `alpha_test` (DungeonWorld_Render.cpp:429, :532). This is latent, because no door or button uses those fields.

Fix: build a per-kind parts list once and have every draw, bake and preview read it. gfx::PreviewSubmesh needs a per-part transform first. The alternative is to reuse the asset picker's PoolModelLook/AddContext, which already shows trim and openers. The preview gaps are cosmetic.

### C180. A rune in a wall niche glows from the floor, even with the niche shut
*medium - bug - effort S*

The floor-glow loop places its light at the floor slot and never checks for a niche (src/Game/DungeonWorld.cpp:848). Every other site handles niches correctly: the draw (src/Game/DungeonWorld_Render.cpp:564), the torch light (src/Game/DungeonWorld_Light.cpp:359) and picking all hide shut-niche items.

So a secret niche gives itself away with a pulsing light at the foot of its wall. It is latent: no shipped level places a niche item, though the editor can.

Fix: add one `FloorItemPose` helper (it returns false when the item is hidden and also handles `upright`) and use it at all five sites. FloorTorchHead has already drifted, since it omits `upright` (Render.cpp:290).

### C181. Fire-light loop dereferences PushLight's null result
*medium - bug - effort S*

PushLight returns null once 256 candidates are queued (src/Game/DungeonWorld_Light.cpp:142), but its header promises null only for brightness 0 (src/Game/DungeonWorld.h:4007). The fire loop writes through `light` without a check (src/Game/DungeonWorld.cpp:799), unlike every other caller.

It is latent. It needs about 240 lit fires, or a great many lit floor torches. Shipped levels have at most 3 fixtures, and the generator places none.

Fix: add the null guard and document both null cases. The ceiling also drops lights in push order rather than by distance.

### C190. Enchanted weapons' floor glow is grey, not their element
*medium - bug - effort S*

ItemKindFor sets an enchanted weapon's glow to its element colour (src/Game/DungeonWorld_Load.cpp:1332). It then overwrites that unconditionally with the category tint at :1404, which is steel grey for weapons. That floor light is the only visible tell for flamebrand and frostbrand.

Reach is low: no level places either blade.

Fix: move the category default above the enchantment block, and correct the stale comment at :1402.

### C191. Light-reach and Earth-stone walks ignore doors and bores
*medium - bug - effort S*

Both BFS walks test only IsWalkable (src/Game/DungeonWorld_LightBudget.cpp:83, src/Game/DungeonWorld_SpellLight.cpp:365):

- A light behind a locked door stays a candidate. Without a shadow cube it bleeds through the door, which is the leak this walk exists to stop.
- An Earth stone beside a locked door maps the sealed room and shows its tracks.
- A fire seen through a window into an unreachable room is culled.

Fix: walk a shared light-blocking query, in which shut doors block and bores pass along their axis. Also key the cache on door state, since opening a door does not bump Revision.

### C193. BuildDungeonMeshes frees chunk meshes before draining the GPU
*medium - bug - effort S*

BuildDungeonMeshes clears every chunk, which frees the shared mesh blocks (src/Game/DungeonWorld_Load.cpp:528), and only drains the GPU after that. Two callers do not drain first: `arena` (src/Game/DungeonWorld_Arena.cpp:159) and eval `reset` (src/Game/DungeonWorld_Save.cpp:165). Both can run while frames that drew those chunks are still in flight.

This is a GPU use-after-free race. It is harness-only and latent: the slow CPU build beforehand usually lets the GPU finish, though WARP could expose it.

Fix: add a WaitIdle inside BuildDungeonMeshes just before the clears, so the function that frees is also the one that drains.

### C195. GPU failures abort without the HRESULT or the removal reason
*medium - bug - effort M*

DN_HR logs only the expression text (src/Graphics/D3DUtil.h:18). As a result, out-of-memory and device-removed failures read the same in the log. Nothing calls GetDeviceRemovedReason, and DRED is never enabled (src/Graphics/GraphicsDevice.cpp:40). A TDR, the most common D3D12 death, therefore leaves no evidence. That breaks the Diagnostics rule that a crash must leave evidence.

Fix: print the HRESULT, log the removal reason plus DRED breadcrumbs and page faults to dungeon.log before ReportFatal, and enable DRED in release builds too.

### C196. An untouched Windowed Apply makes the window native-sized and centres it on the primary monitor
*medium - bug - effort S*

The default saved size is 0, which matches no mode. SeedVideoStaging therefore stages index 0, and modes are sorted largest first, so on a fresh install the dropdown shows the native resolution as if it were current. Pressing Apply in Windowed saves that as the client size. SetWindowed then adds the frame and centres on SM_CXSCREEN/SM_CYSCREEN. The result is a window taller than the screen, with the title bar off the top edge and the taskbar covered. It always lands on the primary monitor whatever Monitor says, and every boot repeats it.

- src/Game/GameUI.cpp:1372
- src/Graphics/DisplayEnum.cpp:64
- src/Platform/Window.cpp:61
- src/Game/Game.cpp:1657

Fix: seed Windowed from the live client size (or add a "Current" entry). Clamp the size to the chosen monitor's work area after AdjustWindowRect, and centre on the selected output.

### C197. The chosen GPU is saved by LUID, which is not stable across reboots
*medium - bug - effort M*

`adapter=` stores a packed LUID, but a LUID is only guaranteed unique until restart. When the saved LUID no longer matches, the device quietly falls back to the high-performance GPU. SeedVideoStaging then stages index 0 in EnumAdapters1 order (often the iGPU) instead of the adapter that is running. On a multi-GPU machine, changing only the resolution and pressing Apply now offers a restart, and Yes relaunches on a GPU nobody chose.

- src/Graphics/GraphicsDevice.cpp:64
- src/Game/GameUI.cpp:1353
- src/Game/GameUI.cpp:1401
- src/Graphics/DisplayEnum.h:8

Fix: save a stable identity (vendor, device, subsys and revision IDs plus the description) and resolve it to a LUID at boot. When the saved adapter is not the running one, stage the running one. Correct DisplayEnum.h's claim that the LUID is stable.

A verifier notes the LUID usually changes on reboot, not always, and the claim about TDR recovery is unverified.

### C198. On hybrid laptops the auto-picked dGPU has no outputs, so Borderless does nothing and the monitor lists are empty
*medium - bug - effort M*

The auto adapter is the first HIGH_PERFORMANCE one. On Optimus-style laptops that is a dGPU with no outputs, because the panels hang off the iGPU. ApplyDisplaySettings looks the monitor up only among the running adapter's outputs, so `output` is null and the Borderless branch silently does nothing, both at Apply and at every boot. Monitor and Resolution show a placeholder dash, and Apply saves a 0x0 size. Exclusive only works by accident: EnumOutputs fails and SetFullscreenState gets a null target.

- src/Graphics/GraphicsDevice.cpp:67
- src/Game/Game.cpp:1636
- src/Game/Game.cpp:1660
- src/Game/GameUI.cpp:602

Fix: enumerate monitors separately from the rendering adapter (every adapter's outputs, deduplicated by DeviceName, or EnumDisplayMonitors). Use that list for placement, resolutions and Exclusive.

### C199. The Video tab uses a display list frozen at boot and saves the monitor as an index
*medium - bug - effort M*

`m_adapters` is filled once. The staged picks are re-seeded only by BuildMenu, at boot and on a language rebuild. Opening Settings refreshes neither, and nothing handles WM_DISPLAYCHANGE.

What goes wrong:
- **Monitor changes:** after a monitor is plugged in, unplugged or reordered by a dock, the dropdowns show stale hardware.
- **Saved monitor:** it is saved as an index into the stale list but resolved against a fresh enumeration. It can resolve to a different monitor or to none, and with none Borderless does nothing.
- **Cancelled picks:** a staged pick that was never applied survives leaving and re-entering Settings.
- **Smaller:** the frame-rate labels read RefreshHz before the window moves to its saved monitor. Settings are saved before they are applied. A failed Exclusive warns without the HRESULT and stays saved.

Where:
- src/Game/GameUI.cpp:1350
- src/Game/GameUI.cpp:1417
- src/Game/Game.cpp:1636
- src/Game/Game_Wiring.cpp:181

Fix: keep one display list and refresh it when Settings opens and on display-change messages. Save the monitor by DeviceName, and save only after the change has applied.

Also: DisplayEnum.h:8 and CLAUDE.md both say Main enumerates at boot, and it does not.

A verifier cut the finder's "four enumerators" to two.

### C200. The frame cap works in whole hertz and drifts against fractional refresh rates
*medium - bug - effort S*

RefreshHz returns whole-Hz `dmDisplayFrequency`, so 59.94 Hz reports as 59. FrameCapHz then divides by the present interval in integers: 165 Hz at interval 2 caps at 82 against a real 82.5. The cap is on by default and runs every frame, exclusive fullscreen included. Whenever it is slower than the display, a vblank periodically has no new frame and the last one repeats, which shows as a hitch every 1-2 s. The labels round instead, so they show 83. 144 and 240 Hz divide evenly, which is likely why this has not shown up.

- src/Graphics/GraphicsDevice.cpp:556
- src/Graphics/GraphicsDevice.cpp:576
- src/Graphics/GraphicsDevice.cpp:604
- src/Game/GameUI.cpp:684

Fix:
- Read the exact fractional refresh rate.
- Use slice = qpcFreq * interval * den / num, biased slightly fast so the vblank sets the pace.
- Skip the cap in exclusive fullscreen.
- Use one formula for the labels and the cap.

The "stale value is cosmetic" comment at GraphicsDevice.cpp:547 is out of date, because the cap now reads that value.

### C201. No DPI awareness: scaled displays blur the swapchain and Windowed sizes come out too large
*medium - bug - effort S*

Nothing declares DPI awareness: there is no manifest, no SetProcessDpiAwarenessContext call and no WM_DPICHANGED handler. On a monitor scaled above 100% (most laptop panels and 4K screens) the process sees logical pixels. The swapchain is created at that size and Windows stretches it, so the scene and every font atlas render blurry, at 2/3 resolution on a 150% display. The Video tab's resolutions are physical DXGI mode sizes applied as a logical client size, so a native-size Windowed window comes out larger than the monitor.

- src/Main/CMakeLists.txt:1
- src/Main/Main.cpp:27
- src/Platform/Window.cpp:38
- src/Game/Game.cpp:1657

Fix: declare PerMonitorV2, either in a manifest or as the first call in wWinMain. Use AdjustWindowRectExForDpi, and apply WM_DPICHANGED's suggested rect. The UI already scales by window height.

### C202. The DDS header layout is written out twice, by hand, in the reader and in the baker
*low - cleanup - effort S*

The reader uses byte offsets (4+76, 4+80) with its own constants, and WriteDdsBc7 fills `header[18..20]` by index with its own copies. They agree today, but nothing ties them together and nothing round-trips the bytes. That same kind of drift hid the BC7 pipeline for 3.5 months. Dds.h and MipBaker.h:8 still describe RGBA8 output, and the RGBA8 read branch has no writer.

- src/Assets/Dds.cpp:44
- tools/AssetBaker/MipBaker.cpp:34

Fix: put the writer beside LoadDdsFile on one header struct guarded by `static_assert(offsetof)`. Add a write-then-read check to Bc7Test or RollTest.

This was filed as medium. A verifier lowered it because a rejected .dds now logs a warning.

### C204. Etched symbols have their gold baked into the PNGs, so they skip the per-material ink solve
*medium - bug - effort M*

BuildEtchGlyphs.py bakes the authored dark-stone gold into every etch_*.png. The cut-stone draw tints them only with a grey dim, so the movement pad's and sheet tabs' symbols never go through ResolveInks. On the lighter materials (snows, limestones, marbles), carved words are moved toward pale gold for contrast, but the etched gold stays at about 2-3.5:1. The baked black and white groove walls keep the symbols readable, so this is a consistency and contrast gap rather than an illegible symbol. The script's gold is also a third copy of the ink, and its lit value already differs from Skin.h.

- tools/BuildEtchGlyphs.py:54
- src/UI/Controls.cpp:385
- src/UI/Controls.cpp:2113
- src/UI/Skin.cpp:129

Fix: bake the groove as light-only, plus a separate white gold-floor mask drawn tinted with CarvedGold/CarvedLit. CLAUDE.md says the etch is "light-only so it suits every material", which is not true today and needs correcting.

### C155. PBR texture sets have two separate owners and no shared cache, so one set can be loaded twice
*medium - cleanup - effort M*

Each surface palette variant loads its own maps through LoadPbrSet, with no dedupe even when two palette ids name one set. Props hold a separate copy in `m_propTextures`. Content does share sets: wall_marble and the marble statues both use marble_white. A level that uses both ways holds the set twice, which at 4k is up to ~64 MB, 3 extra SRV slots and a second load. Surface::Holds is all-or-nothing, so two levels sharing 3 of 4 sets reload all 4. Today's crypt levels do not happen to overlap, so the waste is possible but not measured.

- src/Game/DungeonWorld_Load.cpp:397
- src/Game/DungeonWorld_Load.cpp:1726
- src/Game/DungeonWorld.h:2153
- src/Game/DungeonWorld.h:2179

Fix: one PbrSetCache (name + tier -> `shared_ptr<const PbrMaps>`) shared by surfaces and props. Holds then becomes a per-entry lookup, and the quality swap gets one place to re-resolve. Merge PbrMaps and PropTextures.

### C157. "Catalog set or the model's own materials?" is decided four ways in the kind loaders and a fifth in the picker
*medium - structure - effort M*

Each kind decides differently:
- **Monsters:** multi-material when `meshes.size() > 1`.
- **Decorations:** a `multimaterial` flag plus the file extension.
- **Items:** always multi-material.
- **Fixtures:** always meshes[0] plus the set, so a multi-primitive fixture would silently lose parts.

The picker re-derives the rule as `subs.size()==1 && .gltf`. The rules agree for the shipped content, but every new import has to follow its category's rule, or the preview, map icon and world drift apart. MonsterKindFor also always uploads a spare meshes[0] copy that is only used as a non-null guard.

- src/Game/DungeonWorld_Load.cpp:628
- src/Game/DungeonWorld_Load.cpp:1968
- src/Game/DungeonWorld_Load.cpp:2020
- src/Game/DungeonWorld_Models.cpp:214

Fix: one ResolveModelLook used by all four kind factories and LoadPoolModelLook. Add a Parts() accessor so the six draw, icon and preview sites stop re-assembling the parts. Call ModelMesh only on the single-mesh path.

### C159. AssetDialog and the `preview` command keep the old meshes[0] preview that the picker already fixed
*medium - cleanup - effort M*

AssetDialog's model preview uses meshes[0] with only materials[0]'s base colour, the exact bug AssetPicker.cpp:416 says it fixed. Game's preview branch for the dialog sets no aspect and no fit, so a bought rig shows as one small, untextured, stretched piece. Use installed goes through the picker first, so only Duplicate and Import show this. The dev `preview` command does the same, and it reads the whole file just to test that it exists. Both dialogs also carry near-identical texture-set preview code; the dialog hardcodes `_2k`.

- src/Game/AssetDialog.cpp:414
- src/Game/AssetDialog.cpp:372
- src/Game/Game.cpp:2698
- src/Game/Game_DevCommands.cpp:1024

Fix: call the static LoadPoolModelLook and expose fit and aspect the way the picker does. Overlay the live slider values onto every part. Share one texture-set preview helper.

### C160. The font atlas uploads as RGBA8 with a CPU-built mip chain on every glyph commit
*medium - cleanup - effort S*

Font::Commit expands the one-byte coverage atlas to RGBA and builds it through the Texture constructor that copies the image and builds a full mip chain on the CPU. Glyphs only sample alpha at level 0. So VRAM holds 4x the data plus a third more in mips nothing reads, and each commit (a new font size or a new glyph) does that CPU work behind a full GPU drain, around 150 MB of temporary heap at the 4096 cap. Commits are rare, so this costs something per event, not per frame. Fonts are never evicted, so the VRAM waste applies to every (face, size) pair held.

- src/UI/Font.cpp:211
- src/Graphics/Texture.cpp:11
- assets/shaders/sprite.hlsl:55

Fix: upload a single-level MipChain, as BuildTurbidityMap already does. Better still, use an R8 texture with a (1,1,1,R) SRV component mapping, so neither shader path changes.

### C167. GPU objects are freed at once, so safety rests on about 22 hand-placed WaitIdle calls
*medium - structure - effort L*

`Texture::~Texture` releases its resource and puts its SRV slot straight back on the free list (src/Graphics/Texture.cpp:41, src/Graphics/GraphicsDevice.cpp:681). A Mesh drops its buffer the same way (src/Graphics/Mesh.h:46). So every caller has to drain the GPU by hand first. There are about 22 such WaitIdle calls outside Graphics, for example src/Game/DungeonWorld.cpp:374 (once per editor paint), the undo restore, PartyCreationPage and ThumbCache's evict path.

On top of that, `ExecuteImmediate` (src/Graphics/GraphicsDevice.cpp:685) builds a fresh allocator and command list on every call and ends in WaitIdle, so every texture upload stalls the pipeline.

What this costs:
- A forgotten drain is a GPU use-after-free that only the debug layer reports. Another finding names BuildDungeonMeshes (src/Game/DungeonWorld_Load.cpp:528) as a miss.
- Each correct drain causes a hitch.
- Some drains do nothing. Font::Commit's is redundant because the upload already drains. GameUI_Stone's is redundant only when the load succeeds.

Fix: add a fence-tagged retire queue to GraphicsDevice and release entries in BeginFrame once their fence completes. Route the Texture and Mesh destructors and FreeSrv through it. Uploads can later move to an in-frame staging ring.

Against a documented decision: CLAUDE.md's "drain the GPU before overwriting" SRV rule and the WaitIdle-first lifetime convention. A per-site rule made sense at two sites, but at 20+ (plus a cache designed around the stall), structure is cheaper and can't be forgotten.

### C168. The icon-bake rig is raw D3D12 in Game, and two of the three bakes re-type the shared bracket
*medium - structure - effort M*

`EnsureIconBakeTargets` (src/Game/DungeonWorld_Render.cpp:785-813) creates a committed depth resource and a DSV heap. These are the only raw D3D12 calls above Graphics, against the promise at src/Graphics/GraphicsDevice.h:19-20.

BeginItemIconBake/EndItemIconBake (:1074-1114) already provide the whole bracket. Even so, BakeMeshIcon (:1200) and BakeMonsterIcon (:1252) inline their own copies of:
- the barriers and BeginOffscreen
- the halo (`kHaloColor` is declared at :1095, :1215 and :1267)
- the camera, rebuilt at :1235 and :1303 even though IconCamera's comment at :945 calls it the "ONE copy"
- FlushTransparent and the closing barrier

The verifier narrowed this: nothing visibly breaks. ItemFlameUv and RuneFaceUv only project through the item and rune bakes, and those do use IconCamera. ModelPreview's light rig differs from the icon rig for a reason (a different camera and scale).

Fix:
- Now: have the two bakes call the existing pair, and drop the BakeIconFor forwarder.
- Later: add a gfx offscreen target that owns the depth target, the barriers, the glass flush and the back-buffer restore. Build ModelPreview and Texture::RenderTarget on it too (the latter currently creates its own one-descriptor RTV heap per icon).

### C169. Shadow faces upload the full 8 KB frame block each and draw the whole sphere into all six faces
*medium - cleanup - effort M*

`BeginShadowFace` (src/Graphics/Renderer.cpp:577-583) does two wasteful things per cube face:
- It zero-fills and uploads the whole 8000-byte FrameConstants, although shadow.hlsl reads only a 192-byte prefix (viewProj and gShadowLight).
- It rebinds the root signature, PSO and tables.

`CubeFaceViewProj` is private to Renderer.cpp (:61), so RenderShadowMaps (src/Game/DungeonWorld_Render.cpp:134-140) builds one sphere cull per cube and submits it to all six faces.

The worst case is 48 faces x 8 KB = 384 KB of a 4 MB arena whose overflow aborts. The verifier narrowed the cost: the cube cache limits this to revision changes, flicker ticks or an animated caster in range. Chunks often span several faces, so the real redraw factor is well under 6x and the saving falls mostly on props and monsters.

Fix:
- Give shadow.hlsl its own small ShadowConstants cbuffer and C++ struct.
- Bind the pipeline state once per pass.
- Expose the face matrix so each face can use ViewCull::FromFrustum.

### C170. C++/HLSL constants and layouts are mirrored by hand, with silent failure modes
*medium - structure - effort M*

D3DCompile gets no defines and no include handler (src/Graphics/ShaderCompiler.cpp:104). Everything shared between C++ and HLSL is therefore copied by hand:
- the MAX_* and LIGHT_TILE_COUNT `#define`s (assets/shaders/scene.hlsl:6-9, shadow.hlsl:10)
- 8 shadow cube registers plus a switch, and a literal t11 for the ORM map (scene.hlsl:78-105)
- shadow.hlsl's cbuffer prefix
- AcesTonemap and ScreenConstants

Only a few asserts exist (Renderer.cpp:55-57, SpriteBatch.cpp:67). Nothing pins the sizeof or offsetof of FrameConstants, ObjectConstants or assets::Vertex. These changes compile and then fail silently:
- Changing `kMaxDustPuffs` shifts every point light.
- `kMaxPointLights = 48` passes the `<= 64` assert and scrambles the tile masks.
- `kShadowSlots = 10` binds a cube map as the ORM map.
- A rig over 128 joints is clamped with no message (Renderer.cpp:715).

shadow.hlsl's pre-PBR field names have already drifted. That is names only (the layout still lines up), so it is harmless today. There are also stale comments at Renderer.cpp:539 and scene.hlsl:51.

Fix:
- Pass the C++ constants in as D3D_SHADER_MACROs and hash them into the cache key.
- Make the shadow cubes an array placed after the ORM register.
- Add an include handler for a common .hlsli.
- Add sizeof/offsetof static_asserts.

Against a documented decision: CLAUDE.md's "mirrors the registers BY HAND" and "LIGHT_TILE_COUNT mirrors kLightTileCount BY HAND". Macros remove the need to mirror at all.

### C188. Dialog 3D previews use five protocols, arbitrated by hand in Game::Render
*medium - structure - effort M*

Game::Render (src/Game/Game.cpp:2665-2757) picks a preview source in an if/else chain over the asset picker, asset dialog, monster dialog, instance inspector and dev `preview`, filling 14 `pv*` locals. Item details is a separate path (:2801). The result is then blitted at five sites (:2885-2961) under guards that disagree:
- `devPreviewFullscreen` (:2744) excludes only two of the consumers.
- The item-details blit (:2883) lacks the render's test.

The monster dialog's preview state lives in Game (src/Game/Game.h:1199-1208), as does the inspectors' animator, fire and spin. Its animator setup duplicates src/Game/Game_Inspect.cpp:59-69, and `m_previewMonMat` is written but never read. Four caller-side BindBackBuffer restores (Game.cpp:2752, 2756, 2808, 2827) follow ModelPreview::Render and BakeIconFor. One was missed once (see the comment at :2823).

The verifier found that the wrong-image cases need a dev command to reach. Modality hides the rest, so the substance is fragility, not a visible bug.

Fix:
- Return one extended PreviewSpec from a dialog-base virtual, with each dialog owning its own animator.
- Render once and blit once.
- Have ModelPreview::Render restore the back buffer itself.
- Delete the dev `preview` command (superseded by `assetpicker models`) and its stale Game.h:1110 comment.

### C177. The torch flame in the preview is not tonemapped
*low - bug - effort S*

In the LDR ModelPreview, the meshes get ACES and gamma inline (assets/shaders/scene.hlsl:584-587), but particle.hlsl (:36-38) writes raw linear colour. So a lit torch's flame in the fixture inspector (src/Game/Game.cpp:2732-2741) is graded differently from the world and from the sconce beside it.

Fix: add a tonemap-inline flag to the particle constants. That only gives an approximate match, because it tonemaps each particle rather than the blended sum. An exact match needs an HDR preview plus a composite.

### C183. The map's monster head shot shows the bind pose
*low - bug - effort S*

BakeMonsterIcon (src/Game/DungeonWorld_Render.cpp:1279-1311) draws an Animator that never plays a clip, framed on bind-pose bounds. The map icon is therefore a T-pose, while the picker shows the idle.

The verifier checked the other views: the world, the inspector and the picker all agree for shipped content. MonsterPreviewFor's `clips.front()` fallback (src/Game/DungeonWorld_Load.cpp:865) is only latent.

Fix: play the idle clip's first frame and frame on the posed bounds (FitToPose).

### C189. The skinning-palette cache is keyed by a temporary animator's address
*low - bug - effort S*

UploadPalette (src/Graphics/Renderer.cpp:710-721) caches uploads by `palette.data()` for the whole frame. BakeMonsterIcon (src/Game/DungeonWorld_Render.cpp:1308-1311) builds a throwaway Animator for each kind inside one loop (:1136-1140). The next rig of the same size can land at the same address and reuse the previous kind's upload, so the comment "a temp is safe" is wrong.

The visible effect is near nil while rest palettes are close to identity.

Fix: clear the cache in BeginScene for non-HDR passes, or keep one persistent rest animator per kind.

### C156. The texture-set loading pattern is repeated at four sites
*low - cleanup - effort S*

The three loads `stem`, `stem_n` and `stem_mr` appear at four sites:
- src/Game/DungeonWorld_Load.cpp:381
- src/Game/DungeonWorld_Models.cpp:232
- src/Game/AssetPicker.cpp:390
- src/Game/AssetDialog.cpp:376

AssetUtil.cpp also parses stems twice (:212, :228). AssetDialog hardcodes `_2k`, so a set installed only at 1k previews blank.

The verifier narrowed this: LoadPbrSet's "single source" fallback claim holds, and the editor sites pick resolutions differently on purpose. LoadPbrSet's "dies if absent" comment is stale (a missing texture now gets a placeholder).

Fix: add one map-loading helper and one stem parser.

### C161. glow_radial.png is loaded into two textures, and every model-less item gets its own placeholder
*low - cleanup - effort S*

The same image is loaded twice, at src/Game/Game.cpp:921 (torch flame glow) and src/Game/GameUI.cpp:227 (set-hand glow). That is two SRV slots for one file, which repeats the mistake the CloseIcon fix removed. MakeSolidIcon also builds one texture per model-less item (src/Game/Game.cpp:903): 17 textures for 7 categories. Together that wastes about 11 of the 1024 slots.

Fix: load glow_radial through AssetUtil's shared cache (as CloseIcon and ToolbarIcon are) and have both owners borrow it. Make one placeholder per category, or one white texture tinted at draw time.

The verifier narrowed this: MakeHaloTexture and the ParticleBatch sprite are different shapes, not duplicates. One finder proposed a registry to replace the AssetUtil globals. That argues against the documented CloseIcon design, and it is not needed for this fix.

### C162. The world's ParticleBatch is rebuilt on every level load
*low - cleanup - effort S*

The "fires" load task creates a new batch (src/Game/DungeonWorld_Load.cpp:220), and every stair re-runs the load tasks (src/Game/Game.cpp:681). Each rebuild costs a root signature, two PSOs, 3 x 1 MB of upload arenas and the sprite texture's SRV. None of that depends on the level.

This is wasted work, not a lifetime bug: BeginLevelLoad waits for the GPU to go idle first. Fix: create the batch once (in the constructor or on the first load) and let BuildFires reset only its contents.

### C164. Per-frame upload arenas have a fixed size, abort when full, and show no high-water mark
*low - rule - effort S*

UploadAllocator::Allocate DN_ASSERTs on overflow (src/Graphics/UploadAllocator.cpp:18), which aborts in every build. Nothing reports how full an arena is, the way the SRV gauge does.

- ParticleBatch's 1 MB arena holds about 4,850 particles (216 B each). ReserveParticleScratch computes a ceiling for the level but never checks it against the arena.
- Fires are not culled, and ProjectileSystem::Spawn grows past kReservedItems with no cap (src/Game/Projectiles.cpp:44).
- Each shadow face uploads a full ~8 KB FrameConstants when shadow.hlsl reads about 176 B (src/Graphics/Renderer.cpp:578-583).

The verifier found the risk latent: an overflow would take roughly 145+ fires. Merging the arenas would not save memory.

Fix: add a high-water gauge with warnings, clamp-and-count in ParticleBatch (drop the farthest particles), cap Spawn, and upload a short struct of shadow-face constants.

### C165. Game keeps a second ParticleBatch just for the inspector's torch preview
*low - cleanup - effort S*

`m_previewParticles` (src/Game/Game.h:1227) is a whole second ParticleBatch: root signature, two PSOs, 3 MB of arenas and a sprite SRV. The world's batch already keeps an LDR PSO for this exact preview (src/Graphics/ParticleBatch.cpp:96-99). A world exists whenever an inspector is open, and the preview and the scene are else-if branches in Game::Render, so the world's arena could carry the preview's particles.

Fix: pass the world's batch to the preview and delete the second one. This is easier once C162 makes that batch persistent. Moving all arenas into GraphicsDevice is optional: per-owner arenas are the pattern UploadAllocator.h documents.

### C166. Root signature, sampler and PSO setup is repeated in four pipeline owners, and there are two 1x1 white textures
*low - cleanup - effort S*

- **Root signatures:** serialize-and-create appears 4 times (src/Graphics/Renderer.cpp:165, SpriteBatch.cpp:44, ParticleBatch.cpp:44, PostProcess.cpp:72). The serializer's `errors` blob is never logged, so a bad root signature fails with only the expression text. This is the only functional gap.
- **Samplers and PSO defaults:** the linear-clamp sampler is copied three times. The PSO defaults have already diverged: only Renderer and PostProcess set DepthClipEnable, which is harmless today.
- **Small copies:** there are two 1x1 white textures (Renderer.cpp:285, SpriteBatch.cpp:128). BeginFrame repeats BindBackBuffer line for line (src/Graphics/GraphicsDevice.cpp:327 vs :348). GpuProfiler.cpp:44 hand-builds what BufferDesc already returns.

Fix: add D3DUtil helpers (a CreateRootSignature that logs the error, LinearClampSampler, DefaultPsoDesc, SetViewportScissor), keep one shared white texture, and have BeginFrame call BindBackBuffer.

### C171. Glass pixels run the dust raymarch twice
*low - cleanup - effort S*

GlassOutput calls ApplyDust twice on the same ray (assets/shaders/scene.hlsl:467-468). The march (from :327) does 12 steps with a shadow tap per light per step, and its tau and inscatter do not depend on the surface colour. FlushTransparent draws each glass object twice, so a glass pixel pays for four marches where two would do. Only glass pixels pay this.

Fix: split out `DustMarch(worldPos, lights, out tau, out inscatter)`. GlassOutput then returns `added * exp(-tau) + inscatter * (1 - filter)`, which gives exactly the same result.

### C172. scene.hlsl repeats the point-light falloff and the shadow bias
*low - cleanup - effort S*

The falloff window and attenuation are written in Shade (assets/shaders/scene.hlsl:289-290) and again inline in ApplyDust (:357-360). The 0.012 bias appears at :115 and :130. If one copy is retuned, the god rays stop matching the surface lights.

The verifier noted that ShadowFactor's far path already calls ShadowVisibility (:154), so that body is already shared. Fix: add a `LightFalloff` helper and named `kShadowBias` / `kPcfSpread` constants.

### C173. The shadow-cube SRV table assumes eight back-to-back SRV slots, with no check
*low - cleanup - effort S*

The 8 cube SRVs are allocated one at a time (src/Graphics/Renderer.cpp:386-397), but BeginScene binds only `m_shadowSrv[0]` as the table (:539). AllocateSrv takes slots from the free list first. If any texture is freed before the Renderer is built, the slots are no longer consecutive and shadows silently sample the wrong descriptors. It works today only because Main builds the Renderer right after the device.

Fix: add `AllocateSrvRange(count)`, or DN_ASSERT that the indices are consecutive. A missing ~Renderer FreeSrv is harmless.

### C174. Unused Graphics API and constants that are uploaded but never read
*low - cleanup - effort S*

- **No callers:** GraphicsDevice::SrvHeap() (src/Graphics/GraphicsDevice.h:160), SpriteBatch::WhiteTexture(), Camera::Yaw(), ModelPreview::Size() and Party::Yaw(). Camera::ScreenRay's `farPt` is computed and never used (src/Graphics/Camera.h:40).
- **Unused link:** Graphics links Platform but uses none of it (src/Graphics/CMakeLists.txt:17). ARCHITECTURE.md:41 also lists it.
- **gPointLightCount:** declared in scene.hlsl:23 and shadow.hlsl:18, read nowhere. Replace it with padding to keep the layout.
- **Directional light:** uploaded every frame and tested per pixel (scene.hlsl:273), but the only writer sets it to black. Remove it, or mark it reserved for an outdoor tier.
- **ModelPreview::Project** drops the pivot and tilt that Render applies (src/Graphics/ModelPreview.cpp:80). The one caller passes neither, so this is a latent trap.

### C175. Stale comments in the GPU core and shaders
*low - cleanup - effort S*

These comments point the wrong way exactly where a register mistake is silent:

- src/Graphics/Renderer.cpp:539 says `t3..t6`; the table is t3..t10.
- assets/shaders/scene.hlsl:51 says the ORM is t7; it is t11, and t7 is shadow cube 4.
- scene.hlsl:1-3 says Blinn-Phong with a gPointLightCount loop; the shader is GGX and loops the tile masks.
- Renderer.h:4 says "two PSOs"; there are 9. Texture.h:9 says "Always RGBA8"; BC7 is supported.
- Lights.h:21 says the nearest lights are kept; SelectLights ranks by contribution. Lights.h:44 and Renderer.h:50 name AssignShadowSlots / ShadowSlotCache, which are now ShadowScheduler. CLAUDE.md also still says ShadowSlotCache.
- Other stale comments: SpriteBatch.h:34 (Solid) and :151 (BarVertex leaves out extra.z), ModelPreview.h:4, GraphicsDevice.h:19 (raw D3D12 is used in DungeonWorld_Render.cpp:791-811), shadow.hlsl:32-33 (pre-PBR names), and the frame flow in ARCHITECTURE.md:65.

The verifier dropped one claim: the GameSettings static_assert exists (GameSettings.cpp:26).

### C176. Bar colours that are hand-copied between C++ and HLSL only roughly match
*low - cleanup - effort S*

The skill bars' reserve colours (src/Game/CharacterSheet_Lists.cpp:38-41) claim to be bar.hlsl's bright stops, but they differ by 0.05-0.08 (assets/shaders/bar.hlsl:91-96). GuardSlider.cpp:224 says its colours "cannot disagree" with the bar, yet the burn colour comes from bar.hlsl's own copies (:107-108). They match today.

Fix: send each kind's bright stop as the tint from one C++ table that SkillBarColor also reads, and put the burn colour in the unused `extra.w`.

Against a documented decision: the finder also wanted to replace the BarKind literals, the float[8] root constants and the rune 0.45 factor. Those follow CLAUDE.md's hand-mirror convention and are consistent today.

### C182. The asset picker builds its own copy of the prop material instead of calling ApplyPropMaterial
*low - cleanup - effort S*

When a texture set wins, LoadPoolModelLook builds `doubleSided = true` with no parallax (src/Game/DungeonWorld_Models.cpp:255-259). The world uses `doubleSided = !authored` plus ApplyPropMaterial (src/Game/DungeonWorld_Render.cpp:400-401), which adds 0.03 parallax (src/Game/DungeonWorld_Load.cpp:1738) and the kind's overrides. CLAUDE.md promises the picker shows a model as the world draws it, and this copy is where that falls short.

The verifier found that no shipped decoration, item or fixture sets the overrides. So the visible difference is only parallax and back-face culling in a 256 px tile. Fix: pass the prop height scale and the first bound entry's overrides through ApplyPropMaterial.

### C184. Rune tablet's glow and its light pulse together only by copied numbers
*low - cleanup - effort S*

The tablet's glow pulse is hardcoded in `RunePulse` (src/Game/DungeonWorld.cpp:737). Its light now gets its pulse from lights.cat `[floor_glow]` (src/Game/LightProfile.cpp:118). The two stay in step only because both hardcode the same numbers, so editing `pulse_rate` or `pulse_depth` in the type editor puts them out of step. The comment still says `RunePulse` drives the light. A thrown tablet (src/Game/DungeonWorld_Render.cpp:642) copies the floor material as literals and has no pulse.

Fix: compute the glow with `light::PulseAt` from the `floor_glow` profile, and share one tablet material between the floor and thrown draws.

### C185. Projectile particles are sized in metres, and colours have two parsers
*low - cleanup - effort S*

Trails (src/Game/Trail.h:16), spark bursts and splashes (src/Game/Projectiles.cpp:95, :189) use metre literals. Changing `kUnit` would not rescale them the way it rescales FireEffect (src/Game/FireEffect.cpp:15). The SCALE rule is written about models, so this is a gap in its reach, not a breach.

The colour parsers disagree:
- `light::ReadColor` (src/Game/LightProfile.cpp:36) takes exactly 3 tokens.
- `CatalogColor` (src/Game/Catalog.cpp:19) takes 3 or 4.

Catalog.h:106 warns against exactly this. `Trim` is also copied in 5 files.

Fix: author sizes in units and multiply by `kUnit`, and use one shared colour parser.

### C186. One new kind re-bakes every icon in its category, and fires sort particles while off screen
*low - cleanup - effort S*

Creating any kind clears a flag that covers its whole category (src/Game/DungeonWorld_Load.cpp:1462, :1964, monsters :635). The next Render then re-bakes every loaded kind in that category (src/Game/DungeonWorld_Render.cpp:824, :1144), and each monster bake builds a throwaway Animator. Play preloads item kinds, so this only happens when the editor adds a type: a one-frame editor hitch.

Every lit fire also appends and sorts its particles every frame whether it is in view or not (src/Game/DungeonWorld.cpp:512). Torch flames are distance-culled. This is the only part whose cost grows with content.

Fix: a per-kind `iconBaked` flag, and skip submitting particles for off-view fires while still ticking them.

The verifier found that gating spinning icons on visibility would save little: the only one is Brand's dagger, which is normally on screen.

### C187. Shadow scheduler: dead counter, stale cache docs, and an incumbent matched by position
*low - cleanup - effort S*

`m_frameCounter` (src/Game/ShadowScheduler.cpp:100) is written and never read. ShadowScheduler.h:47, src/Graphics/Renderer.h:50 and CLAUDE.md's renderer paragraph still describe a "ShadowSlotCache" that throttles fires to half rate. The code is a 25 Hz wall-clock budget of 2 cubes a frame (`shadowrate`).

"The carried torch always wins slot 0" no longer holds once two members carry lights (a torch plus a Firelight, say). Slots are ranked only by distance with hysteresis.

The incumbent is matched by position even though `PointLight::id` exists. That is cleanup, not a live bug. One real but narrow lag: a carried Firelight's shadow updates only on the flicker cadence while the party walks (:147).

Fix: delete the counter, fix the comments and CLAUDE.md, match by id, and count a move larger than the light's wander as a move.

### C192. Multi-material models upload each part with its own GPU wait, and the picker uploads them twice
*low - cleanup - effort S*

`BuildMultiMaterialModel` (src/Game/DungeonWorld_Load.cpp:1865) uploads each primitive as its own `gfx::Mesh`, with a separate submit and full GPU wait. `gfx::CreateMeshes` was written to remove that cost (src/Graphics/Mesh.h:9).

The asset picker's `LoadPoolModelLook` then uploads split parts again, plus up to 4 depth bands that each copy the full vertex array (src/Game/DungeonWorld_Models.cpp:328, :367). It throws the first upload away (:381). This costs at model load and for up to 2 picker tiles a frame.

Fix: split on the CPU first, then upload everything in one `CreateMeshes` call.

### C203. The rune groove's 0.45 is copied by hand between RuneBaker and scene.hlsl
*low - cleanup - effort S*

RuneBaker writes occlusion as `1 - 0.45 x carve` (tools/AssetBaker/RuneBaker.cpp:308). scene.hlsl divides by a literal 0.45 (assets/shaders/scene.hlsl:534, :536). Change either side and the held tablet's groove glow silently breaks.

Fix: name the constant on both sides with cross-references, as LIGHT_TILE_COUNT / kLightTileCount are. The verifier found the scan's AO is not being lost: the baker never loads it, and the groove dip is the occlusion the tablet wants. So moving the mask to ORM.a is optional.

## 6. Globals

### C208. UI clip is not restored after a nested clip, and a throw leaves every later frame clipped
*medium - bug - effort S*

Widget::Update and Widget::Draw share one file-static clip (`g_clip`, `g_clipRect`). They restore it only `if (g_clip != outer)`. Once a clip is active, both of those are `&g_clipRect`, so an inner clip is never undone. Two consequences:

- **Nesting (latent, no case exists today):** a scrolling list inside a scrolling tab page would leave the list's clip on its later siblings. Controls below the list would vanish and stop taking clicks.
- **A throw under any ScrollArea or tab page:** Main's frame catch (src/Main/Main.cpp:252) carries on and nothing resets the clip. Input in every context then stays cut to that stale rect for the rest of the run. Drawing mostly recovers, because SpriteBatch::Begin resets the GPU scissor.

Where: src/UI/Widget.cpp:19, src/UI/Widget.cpp:120, src/UI/Widget.cpp:145.

Fix: restore every time this widget pushed a clip, using RAII as ScopedClip does (src/UI/Widget.cpp:43). Also clear the clip at the start of UIContext::Update and UIContext::Render. Better still, keep the clip stack on UIContext.

### C205. Shared UI icons are file-scope globals released by hand, plus a global registry in the UI lib
*low - structure - effort M*

The close box, dropdown glyphs and toolbar discs live in anonymous-namespace `unique_ptr`s (src/Game/AssetUtil.cpp:90). ~Game frees them with a manual `ReleaseSharedIcons()` call (src/Game/Game.cpp:502), which must first clear the UI lib's borrowed registry `g_icons` (src/UI/ControlIcons.cpp:8). Lifetime depends on call order, not ownership.

The re-arm "for the adapter-change relaunch" (src/Game/AssetUtil.cpp:150) is dead code, because RestartApp starts a new process.

Fix: keep one shared texture per icon, but have Game own it. Put an icon holder beside ui::FontLibrary, declared before the dialogs, and carry the control glyphs on UIContext. ReleaseSharedIcons, the tried flag and the global registry then go away.

Against a documented decision: CLAUDE.md's dialog close convention names CloseIcon(device) plus ReleaseSharedIcons. The sharing stays; only the owner changes.

Verifiers lowered this from medium. The lifetime is explicit and documented. Moving the icons would not have prevented the character-sheet null-icon bug either, which came from GameUI's own late-loaded pointer.

### C207. The save-list world filter is a process global that goes stale after a world switch
*low - bug - effort S*

In a `-project` run, `g_saveWorldFilter` (src/Game/SaveGame.cpp:847) is set once to the launch world (src/Game/Game.cpp:441) and quietly filters every ListSaves() call. SwitchWorld is allowed in such a run (src/Game/Game_Editor.cpp:187) but never updates the filter. After a switch:

- Load, Continue and the `load` listing show only the first world's saves.
- The editor's save-reference checks (src/Game/Game_Editor.cpp:919, src/Game/Game_Editor.cpp:1155) see none of the new world's saves. A type delete fails to refuse, and a level delete fails to warn.

Fix: make it `ListSaves(std::string_view world = {})`, have Game pass the world, and delete the global. The `g_refusalsLogged` dedup set can stay as it is.

### C206. D3D12 message-throttle state is file-scope, though the callback has a context pointer
*low - cleanup - effort S*

`g_msgMutex`, `g_lastId` and `g_repeat` (src/Graphics/GraphicsDevice.cpp:424) are file-local statics. The callback is registered with a `nullptr` context (src/Graphics/GraphicsDevice.cpp:468), while the related `m_msgCookie` and `m_msgScratch` are already GraphicsDevice members. With one device per process this does no harm.

Fix: move the three into a GraphicsDevice member and pass `this` as the callback context. That is safe, because the destructor unregisters the callback. The IconStudioLights static raised in the same finding is deliberate and fine.

## 7. Allocation is guarded

### C232. Saving a monster type frees GPU data that the reopened instance inspector still draws
*high - bug - effort M*

`ReloadTypeKind` waits for the GPU, clears the monsters, erases the kind and forgets its model file. Game still holds `m_inspectPreview` (raw sub-mesh, skeleton and clip pointers) and `m_previewAnim`, which is built over them. Both are cached so that route-laying can reopen the inspector.

How to reach it with the mouse:
1. Right-click a placed skel_warrior.
2. Click Edit route.
3. Right-click skel_warrior in the palette, change any field, and Save.
4. Press Enter.

The inspector reopens with the stale spec. The next frames then animate and draw freed memory, which means a crash or device removal. skel_warrior has its own model file, so all of it is freed. Only kinds that share skeleton.gltf survive, through their siblings.

The respawn also gives every monster a new runtimeId, so the route and the reopened inspector both name a dead monster. There is a console twin: `typeset` while an inspector or the monster dialog is open. The trigger is an unusual editor sequence.

- src/Game/Game_Inspect.cpp:56
- src/Game/DungeonWorld_Editing.cpp:217
- src/Game/Game.cpp:2454
- src/Game/Game_Wiring.cpp:573

Fix: when route-laying ends, call `OpenInspectorFor` again instead of re-passing the cache. It looks up the live monster and declines if that monster is gone. Also end route-laying on any respawn, and have `typeset` close open inspectors first.

### C210. The exit-stair prompt and the pit-fall step allocate in frames still treated as steady play
*high - rule - effort S*

**Exit stair.** Stepping onto one (crypt1.map:24 has one near the start) goes through `OfferExit` -> `AskYesNo`. `GameUI::OpenConfirm` then rebuilds the confirm tree, with `loc::Format` strings and two std::functions. The state stays Playing, so the end-of-frame disarm never fires. `quiet` has no `PromptActive` term, so the frames under the prompt and the frame that answers No are armed as well.

**Pit.** A pit step copies a std::string into `m_pendingFall` while Playing continues through the plunge.

No AllocTest mode steps on a stair or a pit.

- src/Game/Game_World.cpp:370
- src/Game/GameUI.cpp:1424
- src/Game/Game.cpp:1747
- src/Game/DungeonWorld.cpp:120

Fix:
- Add `!m_ui.PromptActive()` to `quiet`, and disarm at end of frame while a prompt is up. This is one place and covers every prompt.
- Give the pending fall a kept member string that holds its capacity, rather than an index into the map's stairs.
- Add an AllocTest mode that walks onto eval_arena's exit and a pit.

One verifier argued for medium: the effect is a false alarm in debug only. The lever half of the original finding is under C211.

### C211. Every lever press allocates, and a secret-niche reveal rebuilds chunks mid-play
*high - rule - effort M*

`PressButton` always calls `ToggleNichesNamed`, which returns a vector by value. In debug, every lever press therefore allocates, even when no niche matches.

A real reveal calls `RebuildChunksAround` for each touched cell: `WaitIdle`, `BuildDungeonRegion`, then `CreateMeshes`. This is the only play-time chunk rebuild, and it runs in an armed frame. It is latent: no shipped level has a lever or a niche, but both can be authored.

A verifier corrected the claim about `Revision()`. Bumping it is required, because a reveal changes geometry and the shadow cache keys on it. The AI grid refill is pooled now.

- src/Game/DungeonWorld_Doors.cpp:691
- src/Game/DungeonMap.cpp:1087
- src/Game/DungeonWorld.cpp:374

Fix: make `ToggleNichesNamed` return a count or fill a fixed buffer, and return early when no niche has that name. For the reveal itself, either wrap it in a scoped `alloc::Excused` (it happens once per niche) or pre-build each affected chunk's opened variant at load. Add AllocTest -Lever. One verifier argued for medium.

### C218. Lifting a floor item whose id is over 15 characters allocates, because item moves depend on MSVC's small-string buffer
*high - rule - effort S*

`HeldItem::Set` assigns into an unreserved string. Every put swaps buffers with a slot, so the cursor often ends up holding a slot's 15-character small-string buffer. crypt1.ent:9-11 places three potions with ids of 17-21 characters. Lifting one of them in that state allocates in an armed frame, in release too.

A verifier narrowed how often: a grown buffer keeps its capacity, so this happens about once per buffer in circulation, not on every pick-and-put. Item renames work only because every target id is 15 characters or fewer. The comment at src/Game/DungeonWorld_Light.cpp:24 says renames are "equal or shorter", which is wrong (torch -> torch_lit is longer).

- src/Game/Game.cpp:2580
- src/Game/Inventory.h:121

Fix: have the ItemSlot and HeldItem constructors, copy constructor included, reserve a constant such as 31. A reserve done only at load is lost on roster rebuilds. Have catalog load reject longer ids, and fix the comment. AllocTest -Items cannot see this, because it moves short-id runes that start in the pack.

### C219. The party-bar effect strip is never warmed, so a member's first effect builds widgets mid-play
*high - rule - effort S*

CharacterPanel's effect strip is a `ui::Repeater` that grows its pool with `make_unique<EffectIcon>` during layout. Only the sheet warms its repeaters. Controls.h:945 says `Warm` exists for exactly this case, and Character.cpp:132 already reserves the effect list for it.

So each member's first ward, poison or light, and every new high in their effect count, allocates in an armed frame. It starts again after any HUD rebuild. The cost is bounded: guard reports, not a leak.

- src/Game/CharacterPanel.cpp:323
- src/UI/Controls.cpp:2444

Fix: call `m_effects->Warm(fx::kMaxEffects)` in the CharacterPanel constructor. That also covers the Minimal-layout cards.

For the harness, a console `effect` command runs with the console open, which disarms the frame. Use `autocast hold` instead, which releases on the first armed frame, and refuse a PASS unless an effect count rose.

### C214. The first armed frame after a disarm captures no stacks, so a violation there logs nothing
*medium - bug - effort S*

`AllocTrack::BeginFrame` decides whether to capture stacks from the previous frame's arming, and `ArmFrame` never turns capture on. The first armed frame always follows an unarmed one, so it records zero stacks, and `ReportFrame` prints nothing. The harness opens its window on exactly that frame and releases the held autocast there. A FAIL can then say "call sites are in dungeon.log" when there are none.

- src/Core/AllocTrack.cpp:216
- src/Core/AllocTrack.cpp:247
- src/Game/Game.cpp:1809

Fix: start capture in `ArmFrame`, since only the pure `SteadyStateFrame` runs before it. Log "N allocations, no stacks captured" whenever there are violations but nothing was printed.

Against a documented decision: AllocTrack.h:95-99 calls the previous frame's arming "correct in practice". The first-frame window broke that premise.

### C233. A patrol route and an open inspector survive a world switch and edit the next world's monster
*medium - bug - effort S*

`UnloadWorld` never ends route-laying or clears `m_inspectCfg`, and the inspector's Delete closure keeps the old runtimeId. Ids restart at 1 in each world, and the route block runs whenever any map is open, Player mode included.

How to reach it: Edit route, then New world -> Create and open, then press M and then Esc. Save, Delete or grid clicks then hit whichever monster now holds that id, and write that world's .ent.

- src/Game/Game.cpp:577
- src/Game/Game.cpp:2448
- src/Game/Game_Inspect.cpp:74

Fix: reset the editor session in `UnloadWorld`: end the route and the stroke, clear the selection and the inspect config. Also run the route block only in Editor mode.

### C209. The player's M map is exempt from the allocation guard and allocates every frame
*medium - rule - effort M*

`SteadyStateFrame` skips every frame with the map open, Player mode included, even though the world keeps simulating underneath. Each frame, Player mode builds new marker vectors with one std::string per marker, copies the projectile list, and calls `loc::Tr`/`Format`. The simulation under the map is still guarded in ordinary frames. What goes unchecked is the overlay's own work.

- src/Game/Game.cpp:1749
- src/Game/MapView.cpp:1699
- src/Game/DungeonWorld_Remote.cpp:458

Fix: reuse member marker buffers that hold views of the names, use `loc::View`/`FormatLine`, then arm Player mode (not while browsing another level) and add AllocTest -Map.

Against a documented decision: CLAUDE.md's "no console/overlay" rule and docs/world-map.md:144-148. The travel screen's exemption is deliberate there and should stay, but that doc's "no simulation behind it" is false for the M overlay.

### C212. Quest, flag and fumble events allocate in armed frames, and a comment revives the retired event exemption
*medium - rule - effort M*

`OnItemFound` runs on every floor lift. For an item with quest/flag hooks it calls `substr`, builds `"text_" + stage`, copies `Display()` and passes strings by value to the WorldState setters. In debug this happens even on a re-lift that changes nothing. Its comment, "that is its own event", restates the exemption CLAUDE.md retired. It is only a comment, so the guard still reports these allocations.

Other cases:
- `SetFlagOn`'s first set grows an unreserved vector; a lever with `sets=` triggers it.
- A fumble's Drop/Fling copies the held item's id.

- src/Game/Game_World.cpp:484
- src/Game/WorldMap.cpp:107
- src/Game/DungeonWorld_Combat.cpp:596

Fix: delete the comment. Give WorldState string_view setters that find first and only construct a string for a new entry. Pre-size the stores from the catalogs. Add an AllocTest step that lifts crypt_token.

### C213. Most allocation paths are never measured, and CheckAll leaves out whole judges
*medium - rule - effort L*

`checkall RESULT=PASS` covers much less than the docs call CHECKED:
- CheckAll runs only the default AllocTest and `-Hand` (tools/CheckAll.ps1:106-117). The other 16 modes are run by hand (.claude/commands/check-alloc.md:22-24).
- No mode covers monster chase (the arena modes freeze monsters, src/Game/DungeonWorld.cpp:1308), forward steps, party melee and kills, doors and levers, rest, eating and drinking, stairs or the map.
- RollTest (it already has `--self-test`), EditorTest.py, WorldTest.py, LevelBuildTest.py and `Eval.ps1 -SelfTest` are in no tier. PipelineTest.ps1:86 relies on that last one.

Fix, in order of cost:
- Add RollTest to the quick tier.
- Add `Eval.ps1 -SelfTest` to the full tier.
- Add an extended tier that loops the AllocTest modes and runs the Python judges.
- Write new modes: Chase, Fight, Rest, Lever, Consume, Exit, Map.

Against a documented decision: Eval.ps1 and check-eval.md keep Eval out of CheckAll. That argument is about the measurement suites, and `-SelfTest` is pass/fail.

Narrowed by the verifiers: potions.eval and bombs.eval are honestly labelled measurements. The real overclaim is CLAUDE.md's "Checked by" for partycreation.eval and portraits.eval, which nothing judges.

### C215. log:: calls do not excuse themselves, so a warning in a guarded frame trips the allocation guard
*medium - rule - effort S*

Core/Log formats a std::string with no `alloc::Excused` (src/Core/Log.h:35-50), so each reporter has to wrap itself. Several do not:
- ApplyProcs' unknown on-hit effect warning (src/Game/Effect/Effect.cpp:150)
- Detonate's blast-clamp warning
- CheckDamageLedger's violation line (src/Game/DungeonWorld_Ledger.cpp:66-74)
- A profile snapshot that finishes with the console closed (src/Game/DevConsole_Snapshots.cpp:191)

The result is that a typo'd `on_hit` id or a ledger violation also shows up as an allocation-guard FAIL, which aborts under `allocguard strict`.

Fix: put the excuse inside log::Write and the templates. Arguments the caller builds before the call (LedgerSubjectName) still need their own scope.

Against a documented decision: CLAUDE.md says "reporting must excuse ITSELF". Keep that policy, but enforce it in the one function every reporter calls.

### C221. Resizing a panel or the sheet bakes a new font, with a GPU drain, for every pixel size it crosses
*medium - rule - effort M*

A Ctrl-resize drag writes the panel's scale every frame (src/UI/FloatingPanel.cpp:216), and that scale becomes the subtree's fontScale (:387). SnapResize also measures up to five trial scales through EmAt -> FontAt (:150-164). The sheet's root font follows its scale with no debounce (src/Game/GameUI.cpp:2061).

Each new integer size misses FontLibrary, which costs a WaitIdle mid-drag plus an atlas and an SRV slot that are never freed (src/UI/Font.cpp:222). The count is bounded per face, but the sheet's Display heading alone spans about 42-92 px, so one sweep passes the 64-font warning.

Fix: keep font sizes at the drag-start scale until release, and measure trial scales from vertical metrics without baking.

Against a documented decision: FontLibrary.h says fonts are "never evicted, callers pass settled sizes", and docs/ui-panels-plan.md:192-195 estimated about 17 sizes per role. Panel drags are not settled sizes, and rem-derived sizes multiply that count.

### C222. The model cache keeps every model's embedded images in CPU RAM after upload
*medium - rule - effort M*

ModelFile caches the whole ModelData, including `images` and `imageMips` (src/Game/DungeonWorld_Models.cpp:123, src/Assets/Model.h:155-159). Their only reader is BuildMultiMaterialModel, which runs once per file (src/Game/DungeonWorld_Load.cpp:1827-1837). After that the CPU copy just duplicates VRAM for the world's lifetime. A single-mesh model drawn with a catalog texture set loads its images and never uploads them at all.

In shipped levels this pins about 25 MB for skel_warrior plus about 15 MB of item sidecars. No shipped level places the spider or the centipede.

Fix: load the data non-const, build the multi-material model, clear the images, then share the const data. Alternatively, add a LoadOptions flag that skips images for mesh-only users.

### C227. The 64-drop floor headroom is not a bound, and conjured pebbles can outgrow it
*medium - rule - effort M*

PlaceDrop only reuses a collected runtime drop's slot and otherwise pushes back. ReserveDropRoom keeps 64 spare slots, and only at load and save-apply (src/Game/DungeonWorld_Load.cpp:1664-1686).

Rock drops its pebble at the party's feet when both hands are full (src/Game/Spell/Rock.cpp:28). With Tilo's premade kit that is every cast. A player practising Earth with this 2-mana spell passes 64 uncollected drops in one level. The list then grows inside an armed frame, and grows again at each 1.5x step. AllocTest -Hand does drop at the feet, but never reaches the headroom.

Fix: cap conjured drops with a fixed pool that recycles the oldest uncollected one, or at least log the first growth.

### C228. ResetRoster's resize path drops new members' effect-list reservations
*medium - rule - effort S*

When the roster size changes, ResetRoster does `m_characters = fresh;` (src/Game/Game.cpp:1007-1010). Growing the vector copy-constructs the new members, and a copied `effects` vector has capacity 0. That throws away the `fx::ReserveEffects` call from CreateDefaultParty (src/Game/Character.cpp:135).

LoadGame always resets to the default four (src/Game/Game.cpp:1268). So after playing a created party of 1-3, loading a 4-member save or starting the default party leaves the added members unreserved. Their first burn, poison or ward then allocates in an armed frame. AllocTest -Party only ever shrinks the roster, so it never sees this.

Fix: reserve every member's effects after both assignment paths in ResetRoster.

### C236. UnloadWorld is a hand-kept list of borrowers, and in-world reloads have nothing like it
*medium - structure - effort L*

UnloadWorld closes about 20 dialogs one by one, all seven inspectors included, even though `InstanceInspectors()` exists for that loop (src/Game/Game.cpp:583-621, src/Game/Game_Inspect.cpp:16). It also leaves two things behind:
- `m_previewMonSubs` and `m_previewAnim` still point into the dead world (src/Game/Game.h:1199-1205). They are inert only because other flags gate them.
- A running bake survives the unload (see C234).

In-world reloads have no hook at all. One hole is confirmed: saving a monster type from the palette while laying a route calls ReloadTypeKind (src/Game/Game_Wiring.cpp:573). That erases the kind that `m_inspectPreview` and `m_previewAnim` point into.

Fix: a `WorldSession` object reset before the world, an `OnWorldGone()` registry for dialogs, and a `ContentEpoch()` that borrowers check. A cheap first step is to loop the inspectors and cancel the bake on unload.

### C226. stack::SeenSet reports every new stack again forever once it is full
*low - bug - effort S*

Once its 64 entries are full, FirstSighting returns true for every unstored hash (src/Core/StackTrace.cpp:189-194). Its header promises the opposite. The allocation guard then logs and symbolizes a stack on every violating frame (src/Core/AllocTrack.cpp:249-257). This needs 64 distinct sites in one debug session.

Fix: return false when full, and log a single "N further sites" line.

### C234. A console world switch during a bake writes the new type into the wrong world
*low - bug - effort S*

SwitchWorld never checks `m_baking` (src/Game/Game_Editor.cpp:187). When the bake ends, FinishBake writes the catalog entry and imports.cat record into whichever world is loaded at that moment. It can replace a type with the same id (src/Game/Game_Editor.cpp:490-572). Only reachable from the console (`worlds load`).

Fix: refuse a world switch while a bake is running.

### C235. Type editor theme swatches hold raw albedo pointers that a quality change frees
*low - bug - effort S*

Theme rows keep a raw `gfx::Texture*` swatch for the page's lifetime (src/Game/TypeEditorDialog.cpp:483-490). Running console `quality low` while the editor is open reloads the surface textures (src/Game/DungeonWorld_Load.cpp:417), and the dialog then draws freed textures. Only reachable from the console.

Fix: resolve the swatch at draw time instead of storing it.

### C216. AllocTest -Pause overstates its coverage, and three comments are stale
*low - cleanup - effort S*

The -Pause header (tools/AllocTest.ps1:115-125) and check-alloc.md:37-41 claim to check the frames after a resume. Those frames fall inside the documented 120-frame warm-up, as the script admits at :1663-1666, so this is a doc fix. Also stale: AllocTest.ps1:896 and src/Core/ThreadManager.cpp:160, which lists a supervisor that never registers.

### C217. Two allocation excuses are not reporting: the Help button line and ReturnToTitle
*low - rule - effort S*

MoveKeysHelp excuses itself as reporting (src/Game/GameSettings.cpp:400-410), but it builds the player's Help-button log line. ReturnToTitle's whole-body excuse (src/Game/Game.cpp:1536) is redundant, because that frame is already disarmed.

Fix: cache the key names and use loc::FormatLine, and drop the ReturnToTitle excuse.

Against a documented decision: docs/lighting-updates-plan.md records the MoveKeysHelp excuse, and that conflicts with docs/message-allocation.md.

### C220. Some warm-up sizes are smaller than what fills them (32 spell rows for 44 spells; 160-byte labels given 255-byte lines)
*low - rule - effort S*

The sheet warms 32 spell rows (src/Game/CharacterSheet.cpp:52), but the registry has 44 spells: 20 base spells plus 24 ModifiedSpell variants (src/Game/Spell/AllSpells.cpp:74). The comment there still says "16 in the demo". If one member learns 33 or more spells, the next sheet or party-window open grows RowPool and the repeater inside a guarded frame (src/Game/CharacterSheet.h:447). That happens once for each new highest count, not on every open. Separately, ItemDetailsDialog's `kValueCap = 160` (src/Game/ItemDetailsDialog.cpp:44) receives resist and cure lines of up to 255 bytes.

Fix: size the warm-up from the registry, set kValueCap to `loc::Line::kCapacity`, and fix the comment. The verifier notes that no shipped level places a rune tablet, so for now this is far out of reach.

### C223. The `uitree` overlay allocates every frame, and the allocation guard does not know it is on
*low - rule - effort S*

When `uitree` is on, `inspect::Draw` builds a vector and `std::format` lines on every frame (src/UI/TreeInspector.cpp:114, :139). It is called for every context without an excuse (src/UI/UIContext.cpp:79). SteadyStateFrame's quiet test does not check `inspect::Enabled()` (src/Game/Game.cpp:1747). So if you turn on `uitree`, close the console and hover the HUD, you get false violations, and with `allocguard strict on` the game aborts (src/Core/AllocTrack.cpp:260). This breaks CLAUDE.md's rule that a reporter excuses itself.

Fix: add `alloc::Excused` right after the enabled check. It only affects a dev tool.

### C224. DrawButtonFace takes `const std::string&`, so its callers build a string every frame
*low - rule - effort S*

The label is `const std::string&` (src/UI/Controls.h:1057), while Font::Draw already takes a string_view. As a result:

- TabControl passes `""` for every skinned tab on every frame (src/UI/Controls.cpp:2417).
- WorldMapView copies `loc::Tr` every frame and trims a copy one byte at a time (src/Game/WorldMapView.cpp:335, :386). That trim can cut a UTF-8 character in half.
- The statics `kNoLabel` (src/UI/Controls.cpp:426) and `kNoSelection` exist only to avoid this conversion.

All of these screens are outside the guard, so the problem is latent. Fix: take `std::string_view`, delete the statics, and use `loc::View` and `ui::FitText` in WorldMapView.

### C225. Every CharacterSheet rebuilds its Skills and Effects rows every frame, whatever tab is showing
*low - cleanup - effort S*

`UpdateSelf` calls `BakeSkills()` and `BakeEffects()` unconditionally (src/Game/CharacterSheet.cpp:183). With the party window open, its four card sheets (src/Game/PartyWindow.cpp:61) mean five sheets do this every frame, even on the Inventory or Stats tab. Each effect row runs two `loc::FormatLine` calls (src/Game/CharacterSheet_Lists.cpp:255). Nothing is allocated, so the guard is not involved. It is just wasted work, and the rest of the UI only rebuilds on a change. Fix: bake only while the Skills or Effects tab is visible, or key the bake on a cheap revision counter.

### C229. Missing font glyphs are baked mid-play under an excuse, not pre-warmed
*low - rule - effort S*

Rebake only pre-warms code points 32-255 (src/UI/Font.cpp:121). Some text uses characters outside that range: en.lang has 56 em-dashes, including the effect tooltip at assets/lang/en.lang:465, and ru.lang's letters are all outside it. The first time such a character shows at a given font size, EnsureGlyph and Commit (src/UI/Font.cpp:156, :203) bake it, re-upload the whole atlas and call `WaitIdle` inside a play frame, and the guard is excused from seeing it. This costs once per glyph per size. CommitAll (src/Game/GameUI.cpp:2070) batches each frame's misses into one upload.

Fix: when a language loads, pre-warm the code points its .lang file uses, then remove the excuses so any remaining miss gets reported.

Against a documented decision: src/Core/AllocTrack.h:22-23 names "a first-time bake" as an allowed excused scope. The verifier agrees that the part worth acting on is the mid-play GPU stall.

### C230. Three documented reserve sizes are not real limits (projectiles, palette cache, sprite vertices)
*low - cleanup - effort S*

Each of these reserves a typical size and then calls push_back with no capacity check:

- `ProjectileSystem::Spawn` reserves 64 (src/Game/Projectiles.cpp:44), and `BillboardCeiling` (src/Game/DungeonWorld.cpp:576) relies on that 64.
- The renderer's palette cache (src/Graphics/Renderer.cpp:719).
- SpriteBatch's pending vertices (src/Graphics/SpriteBatch.cpp:214).

A heavy volley, a crowded room or a big log grows them once inside an armed frame.

Fix: SpriteBatch can Flush at capacity, which is free. Size the palette cache from the monster count at level load. Projectiles need a cap with a way to land the item: the verifier points out that thrown cargo must never be lost, so the system cannot simply refuse a launch.

### C231. Some C-API boundaries are not RAII-wrapped (FILE*, stb/dr_wav buffers, IXAudio2)
*low - rule - effort S*

CLAUDE.md says FILE* and the other C-API boundaries are RAII-wrapped, but the engine's main file reader is not. `ReadBinaryFile` closes a raw `FILE*` by hand (src/Assets/File.cpp:10) and never checks `ftell`. A -1 becomes a SIZE_MAX vector, which throws length_error and leaks the handle. Image.cpp and Wav.cpp copy out of the C buffer before freeing it (src/Assets/Image.cpp:15, src/Assets/Wav.cpp:18), so a throwing copy leaks the buffer. AudioEngine and Profile.cpp's DumpTrace use a raw `IXAudio2*` and a raw `FILE*`. All of these leak only on exception paths.

Fix: use unique_ptr deleters as ObjLoader and SaveGame already do, use ComPtr for XAudio2, and check `ftell`.

### C237. About 360 KB of profiling-only storage sits inside DevConsole in every build
*low - cleanup - effort M*

DevConsole holds fixed arrays inline: snapshots (about 295 KB), profile series, smoothing and health rows, about 390 KB in total (src/Game/DevConsole.h:475, :500). Without DN_PROFILE, about 360 KB of that is never written, because the writers are stubs (src/Game/DevConsole_Snapshots.cpp:293). Game holds DevConsole by value (src/Game/Game.h:1108), and Game is a local in wWinMain (src/Main/Main.cpp:105) with the default 1 MB stack, so this takes close to 40% of the main thread's stack.

The verifier found no sign of real stack pressure, and CrashHandler already dumps on a stack overflow, so this is a cleanup rather than a hazard. Fix: allocate the readout state once in the constructor, or compile it only under DN_PROFILE, or heap-allocate Game.

## 8. Repetitive and redundant code

### C253. The glTF node-transform bake is copied at seven sites with a shared normal/winding flaw, and the multimaterial cull radius skips it
*medium - bug - effort M*

LoadGltf stores `worldTransform` but leaves vertices in node space, so each consumer bakes it by hand:
- full copies: src/Game/DungeonWorld_Load.cpp:1133 (`BakeNodeTransform`), the same loop re-inlined at :1850, src/Game/DungeonWorld_Models.cpp:288 and tools/AssetBaker/ModelImport.cpp:72
- position-only copies: Load.cpp:1898 and Models.cpp:198 and :449

None of the copies uses the inverse-transpose for normals, and none flips winding when det < 0. A non-uniform or mirrored node would come out skewed, or inside-out under CULL_BACK. This is latent: the current assets probably have no such nodes.

`ModelOriginRadius` (Load.cpp:1918, used at :1973) reads raw positions. viking_dagger's cull sphere is therefore about 115x too big, so the four multimaterial daggers are never culled. No level places them yet, and the error is on the safe side.

Fix: add one `assets::BakeNodeTransform(MeshData&)` that uses the inverse-transpose, flips winding on a negative determinant and skips skinned meshes (glTF says to ignore their node transform; BuildMultiMaterialModel and FitToPose currently bake it anyway). Take the multimaterial cull radius from `multi->boundsMin/Max`. Verifier notes: the bind-pose fit at Load.cpp:846 is correct, and the Model.h comment is not stale.

### C238. Core/StringUtil has no Trim or split, so they are rewritten in about 20 places
*medium - cleanup - effort M*

src/Core/StringUtil.h:6 holds only Widen/Narrow, so every parser brings its own:
- **Trim:** five copies (src/Core/Loc.cpp:31, src/Game/LightProfile.cpp:18, src/Game/Serialize.cpp:15, src/Game/Style.cpp:12, src/Game/Trail.cpp:15). Style's copy does not strip '\r'. That is harmless today because Serialize has already trimmed the value.
- **Splitters:** Balance `Tokens`, Load `SplitTokens`, the Game.cpp:931 lambda, DevConsole `Tokenize` (identical to Project `SplitWords`), TypeEditor `SplitOptions`, PartyRules `SplitList`, and `Game::SplitKnobs` as a static member (Game.h:617).
- **First-word and join:** two first-word helpers, and the join lambda about 17 times.

The pure TUs can use Core, since RollTest links it. Fix: add Trim, FirstWord, SplitWords (into string_views), SplitList(seps) and Join to `str::`, keeping the separator a parameter where formats really differ.

### C240. No shared number parser: record loaders repeat the strict parse, dev commands use unchecked atoi/atof
*medium - cleanup - effort M*

**Record loaders.** src/Game/DungeonMap.cpp re-types the same "from_chars the whole token or DN_ASSERT" lambda about eight times (:143, :233, :506, :560 ...), and Entity.cpp:107 and WorldMap.cpp:26/35 repeat it. Two `FloatOf`s disagree: src/Game/Balance.cpp:168 keeps the fallback, while SaveGame.cpp:24 turns garbage into 0. Reframe's `shiftCell` (src/Game/DungeonEntities.cpp:90) accepts `leashfrom=5,abc` as a square, against its own comment.

**Dev commands.** About 150 atoi/atof/strtof calls. `timescale abc` sets 0 and freezes the world (src/Game/Game_DevEval.cpp:36). `tp x 5` acts on column 0, and `tp 1O 5` acts on the prefix 1. This defeats devargs::Need's refuse-don't-measure rule.

Fix: add strict `str::ParseInt/ParseFloat`, one `RequireInt` wrapper for records, and `devargs::Int/Float` that refuse a bad token. Make ParseCell the only "x,z" parser. Also fix the stale "~30 commands" comment in DevCommandArgs.h:20.

### C241. About 22 dev commands hand-parse a member index, so a typo targets member 0
*medium - cleanup - effort S*

Each member command does `static_cast<size_t>(std::atoi(args[0].c_str()))` plus its own range check. "no such member" appears 21 times in src/Game/Game_DevParty.cpp (e.g. :37, :70), plus Game_DevDiagnostics.cpp:144 and Game_DevEval.cpp:339. A non-numeric token such as `learn x fire` acts on Brand: the "measured whatever the world held" failure that devargs::Need exists to refuse.

Related copies:
- `args[0] == "on" || args[0] == "1"` at Game_DevEval.cpp:67/205/294/433 and Game_DevWorld.cpp:57, although `devargs::ArgOn` (src/Game/DevCommandArgs.h:55) exists.
- The sheet tab name table twice (Game_DevParty.cpp:955, :1128), plus a bare `t < 5`.

Fix: add `devargs::Member(...)` that refuses non-numbers, use ArgOn everywhere, and give CharacterSheet one ModeName table with a static_assert. The direction parsing is covered in C242.

### C242. Direction, archetype and surface token tables are re-declared in many places, and facings are parsed four ways
*medium - cleanup - effort S*

**Direction names.** `DirToken` exists (src/Game/Entity.h:41), yet {north, east, south, west} is re-declared at DungeonWorld_Doors.cpp:641, DungeonWorld_Editing.cpp:514, DungeonWorld_SpellLight.cpp:522, Game_DevCommands.cpp:155 and Game_Generate.cpp:65. The last one has a comment wrongly saying no shared helper exists. Party::FacingName duplicates FacingLocKey.

**Archetypes.** Four token tables (src/Game/DungeonWorld_LevelIO.cpp:371, Game_Editor.cpp:1243 and two dropdowns) plus an if-chain parser. The writers index by the enum with no static_assert, so a seventh archetype missed in one table would write garbage into a .ent or monsters.cat on `savemap`.

**Dev-command facings.** `face` refuses a bad letter, `tracks add` falls back to North, `spawn` falls back to South, and `editor place` accepts full names only. {wall, floor, ceiling} is written four times.

**Door open flag.** Validate.cpp:190 reads `open` as `== "1"`, while the loader and MapView use `!= "0"`. A keyed door authored `open=true` loads open but the checker treats it as shut, a false finding.

Fix: ArchetypeToken with a static_assert in MonsterAI.h, DirToken everywhere, one ParseDirArg, SurfaceToken, and DoorAuthoredOpen. Leave the local step-offset arrays alone: they are order-sensitive, and the verifier found they are not worth sharing.

### C249. The allocation-free text helpers (tenths, loc key builders, text fit) are copied per file and have drifted
*medium - cleanup - effort S*

- **Tenths formatter:** four copies (src/Game/ItemDetailsDialog.cpp:49, CharacterSheet_Status.cpp:111, CharacterSheet_Inventory.cpp:285 and :414). They already handle negatives differently; two clamp at zero on purpose.
- **Loc key builders:** three hand-built `<prefix><id><suffix>` buffers: HintFor (Status.cpp:36), `Lookup` (ItemDetailsDialog.cpp:61) and the .desc key (CharacterSheet_Lists.cpp:268). Four of Lookup's five callers pass no suffix, which is exactly `loc::ViewKey` (src/Core/Loc.h:93).
- **Text fit:** `FitWidth` (Status.cpp:51, "...") and `ui::FitText` (".."), so the same status line trims differently on the sheet and in the party window (src/Game/PartyWindow.cpp:164).
- **Bag weight:** "a bag weighs its contents" is coded three times (CarryLoad, SetItemStatus, GameUI::ItemWeightAt).

Fix: add `FormatTenths(span<char>, float)`, a three-part `loc::ViewKey`, and an Inventory weight helper, and delete FitWidth.

### C251. The first-solid-wall mount scan is copied about eight times
*medium - cleanup - effort S*

The N/E/S/W "first non-walkable neighbour" loop decides which wall a sconce, lever or banner hangs on, so every copy has to match the 'T' sconce rule. Nothing ties them together. Copies:
- src/Game/DungeonMap.cpp:444 (sconce resolve), plus FreeSconceWall :880 and FreeNicheWall :941 with a "taken" filter
- src/Game/DungeonWorld_Doors.cpp:518 and :560 (AddButton, AddButtonRemote)
- DungeonWorld_Editing.cpp:69, DungeonWorld_Remote.cpp:418, and DungeonWorld_Move.cpp:30/129 (with a preferred side)

DungeonMap already owns the inverse, OpenFacing.

Fix: add `DungeonMap::FirstSolidWall(x, z, out, prefer, taken)` and call it from all of them. Also have MapEditor's flood (MapEditor.cpp:1097) reuse area::Region with a predicate. Verifier narrowing: the other BFS loops and step tables differ for real reasons. DirDX/DirDZ live out of line in Entity.cpp, which RollTest's pure TUs (Blast, Generate) do not link, so "use DirDX everywhere" first needs them as inline constexpr.

### C254. The mesh-builder call with five resolver lambdas is copied three times
*medium - cleanup - effort S*

RebuildChunkRegion (src/Game/DungeonWorld.cpp:334), GeometryFingerprint (src/Game/DungeonWorld_Load.cpp:454) and BuildDungeonMeshes (:517) pass the same block sets and three `uAspect`s, plus five identical lambdas: holes, niche, bore, floor feature and ceiling feature. These are the only callers outside DungeonMeshBuilder. BuildDungeonRegion takes 14 parameters and StampCell 18 (src/Game/DungeonMeshBuilder.h:134, :152), so a new resolver means three signature edits and three call-site edits.

Verifier correction: if the copies drifted, `geomhash`/`geomlayout` would most likely report STALE (a false alarm) rather than hide a defect. The cost is maintenance.

Fix: add a `SurfaceBuildInputs` struct in DungeonMeshBuilder.h, filled by one DungeonWorld method and taken by all three sites and StampCell.

### C255. What blocks a square is coded three times (party, monster slot, AI grid)
*medium - cleanup - effort M*

Brazier, closed door, blocking decoration and the party are checked by hand in three places:
- the party's `isOccupied` lambda (src/Game/DungeonWorld.cpp:145)
- FreeSlotInCell (:2311)
- BuildAISnapshot: walkable+brazier in the revision-cached grid (:2084), decorations, doors and the party per frame (:2124)

`SolidDecorationAt` (:1689) repeats the lambda's loop, and its declaration says so (src/Game/DungeonWorld.h:3878). They agree today and differ for real reasons (per-blocker messages and sounds, cached versus per-frame grid). But the next blocker, such as a portcullis, can be missed in one copy, and then monsters path through what the party bumps into, or the reverse.

Fix: one `BlockerAt(x, z)` that returns the blocker KIND, with each kind tagged map-revisioned or per-frame so the AI keeps its documented split.

### C257. The decoration world transform is rebuilt by hand at seven sites
*medium - cleanup - effort S*

`UnitScale(kind.modelScale) * XMMatrixRotationY(yaw) * XMMatrixTranslation(pos.x, 0, pos.z)` is written eight times across six functions. Each one picks wall-mount or centre by hand:
- src/Game/DungeonWorld_Move.cpp:215/219
- src/Game/DungeonWorld_Editing.cpp:104/414/439
- src/Game/DungeonWorld_Load.cpp:2079/2084
- src/Game/DungeonWorld.cpp:1812 (SetDecorationFacing)

Nothing owns the rule. A per-kind vertical offset, wall inset or scale change missed at one site would show as a prop that jumps after a move or a re-face.

Fix: add one `PlaceDecoration(Decoration&)` that derives `world` from kind, cell, facing and wall mount, and call it from load, add, move, remount and re-face.

### C261. The button push clock is written three times, and the carved-stone face four
*medium - cleanup - effort M*

ui::Button (src/UI/Controls.cpp:315-358), SlotRow (:1692-1748) and MenuList (:1968-2030) each have their own sink/hold/rise state and their own `Depth()`. The identical cancelled-rise `duration_cast` expression appears at :354, :1745 and :2025.

The copies have already diverged:
- fireOnPress and PressVisual exist only on Button.
- SlotRow and MenuList fire on release only.
- The SlotList confirm buttons draw at depth 0 and never push (:1917).

DrawCutStone and DrawCarvedText are already shared. What repeats is the centring, the `depth * max(1, h * 0.035)` sink and the selection hairline. So "every button reads alike" is kept up by copy-paste.

Fix: extract a `PushClock` (Press/Release/Cancel/Step plus Depth). Then either build the rows from `ui::Button` with `carved`, or share one `DrawCarvedFace` helper.

### C239. ASCII-lowercase, contains-no-case and whitespace-split are copied about a dozen times
*low - cleanup - effort S*

- **Lowercase:** three file-local `Lower`s (src/Assets/PbrMaps.cpp:17, src/Game/AssetPicker.cpp:48, src/Game/DevConsole_Commands.cpp:60) plus about 12 inline tolower transforms.
- **Split:** DevConsole `Tokenize` (:33) is byte-for-byte Project.cpp:89 `SplitWords`.

One real gap: the .map `tags` loop (src/Game/DungeonMap.cpp:350) lowercases but does not split commas the way Catalog's ParseTags does.

Fix: add ToLower, ContainsNoCase and SplitWords to `str::` (overlaps C238). Delete the dead Model branch at src/Game/Game_Wiring.cpp:472, or give InstalledModels an extension filter: it lists sidecar stems, which is harmless today.

### C243. Facing tables, kPi and the yaw wrap are copied outside Entity.h
*low - cleanup - effort S*

Two tables indexed by facing repeat `DirDX`/`DirDZ`: `kFrontDX/DZ` at src/Game/DungeonWorld.cpp:750 and Party's `kDirX/kDirZ` at src/Game/Party.cpp:13. Party's `YawForFacing` (:37) and `FacingName` (:112) also repeat `DirYaw` and `FacingLocKey` (src/Game/Entity.cpp:36, :45).

Other small copies:
- Local `kPi` constants (DungeonWorld.cpp:1294, MonsterAI.cpp:133, MapView.cpp:34, FloatingPanel.cpp:19) repeat src/Core/MathTypes.h:29.
- The yaw-wrap loop appears at DungeonWorld.cpp:1296 and MonsterAI.cpp:138.
- The reserve at DungeonWorld.cpp:186 is dead, because :189 reserves more.
- `isOccupied`'s decoration loop (:164) could just call `SolidDecorationAt` (:1689).

Fix: send the two facing tables through `DirDX/DirDZ`, and use Core's `kPi` with one shared wrap helper.

Verifier narrowed: the other ~11 tables only walk a cell's four neighbours, where order does not matter. They are not facing copies. Blast.cpp is linked into RollTest without Entity.cpp, so a shared table for those would have to be a tiny constexpr header.

### C244. FNV-1a, fixed-buffer copy and UTF-8 widening are rewritten across Core
*low - cleanup - effort S*

FNV-1a is written four times: src/Core/Diagnostics.cpp:102, src/Core/StackTrace.cpp:180, src/Graphics/ShaderCompiler.cpp:32 and src/Game/DungeonWorld_Load.cpp:465. Both Core copies use `1469598103934665603`, which is the real offset basis without its final 7, so they are not true FNV-1a. They only hash for identity within a run, so nothing breaks.

Two more repeats:
- The bounded copy into `char[N]` appears six times (CrashHandler.cpp:45, Diagnostics.cpp:144, AllocTrack.cpp:152, Profile.cpp:160 and :265, Loc.cpp:108). Only some have the `if (n)` guard.
- `SetOsThreadName` (src/Core/ThreadManager.cpp:48) repeats `str::Widen`.

Fix: add a Core/Hash.h with the correct basis and a `CopyFixed` helper, and call `str::Widen`. `stack::Hash` folds 64-bit words rather than bytes, so the shared part is the constants plus a step function.

### C245. gfx::Rect only has Contains, so Intersect and Inset get rewritten
*low - cleanup - effort S*

`gfx::Rect` (src/Graphics/SpriteBatch.h:26) only offers `Contains`. `Intersect`/`Intersects` are private to src/UI/Widget.cpp:22, and src/UI/TreeInspector.cpp:179 works out the overlap again. The inset `{r.x + in, r.y + in, r.w - 2*in, r.h - 2*in}` is written out 14 times, for example src/Game/PartyHudDraw.cpp:210 and src/UI/Controls.cpp:290.

Fix: add `Inset`, `Intersect` and `Overlaps` members to the struct and replace the copies.

### C246. Small math copies: Vec4 Mix, inline smoothstep, vector length
*low - cleanup - effort S*

- An identical `Vec4 Mix` sits in src/Game/GuardSlider.cpp:39 and src/UI/Controls.cpp:2060, because src/Core/MathTypes.h:41 only has `Lerp` for float and Vec3.
- src/Animation/Animator.cpp:92 and src/Game/DungeonWorld.cpp:1224 inline a smoothstep that is exactly `Ease(Easing::EaseInOut, t)` (src/Core/Easing.h:75).
- `sqrt(x*x+y*y+z*z)` is inlined at 7 sites, mostly lengths and distances.

Fix: add a Vec4 `Lerp` and Vec3 `Length`/`Normalize` to MathTypes.h, and call `Ease`. (The `Widen` copy is covered in C244.)

### C247. The probability roll is repeated beside generate's Chance()
*low - cleanup - effort S*

src/Game/Generate.cpp:84 has a file-local `Chance(rng, p)` with a `p > 0` guard. src/Game/Magic.cpp:70 (fumble) and src/Game/Game_World.cpp:415 (encounter) write the same roll by hand. src/Game/Effect/Effect.cpp:146 uses the inverted form `roll > chance`, so a proc with chance 0 still fires on an exact 0.0 draw. That is very unlikely, but it is a different rule.

Fix: put `Chance`/`Uniform01` in the pure Game/Roll.h, which RollTest links. Only line 87 of Carve.cpp fits it.

### C248. Reading a whole file as text is open-coded everywhere
*low - cleanup - effort S*

src/Assets/File.h:11 only offers `ReadBinaryFile`, so the bytes-to-string step is repeated at Catalog.cpp:94, GameSettings.cpp:161, Project.cpp:106 and :142, GameUI_Stone.cpp:206 and Game_DevWorld.cpp:606. src/Game/Game_NewWorld.cpp:350-358 and src/Game/Game_Populate.cpp:68-76 share an identical "level text, else disk" block.

Fix: add `assets::ReadTextFile` and a separate `LevelTextOrDisk`. Do not put the fallback inside `LevelTextFor`: Game_NewWorld.cpp:332 wants in-memory text only, because the disk files were already copied.

### C250. Five hand-rolled fixed-capacity inline string types
*low - cleanup - effort M*

These five types all exist to avoid debug-CRT allocation:
- `EffectId` (src/Game/Effect/Effect.h:80)
- `SpellIdList`'s slot (src/Game/SpellIdList.h:76)
- `UseDefaults`' slot (src/Game/UseDefaults.h:86)
- `loc::Line` (src/Core/Loc.h:66)
- `ContextMenu::Row` (src/UI/Controls.h:581)

Each one decides overflow and aliasing by hand. Three refuse overlong input and two truncate, which is deliberate.

Fix: add a Core `InlineString<N, Overflow::Refuse|Truncate>` with memmove semantics and rebuild the five on it.

Verifier note: `UseDefaults::SetItem`'s memcpy is safe, because it only writes into a fresh local.

### C252. The mesh-vertex bounds loop is hand-written about ten times
*low - cleanup - effort S*

About ten sites run the same plain min/max loop over vertex positions, for example src/Game/DungeonWorld_Load.cpp:846, src/Game/DungeonWorld_Render.cpp:1124 and :1279, src/Game/Liquid.cpp:36, and tools/AssetBaker/ModelImport.cpp:102. `FillItemPreview` (DungeonWorld_Load.cpp:971) rebuilds the rune-tablet bounds already cached at :1422, but only when a dialog opens.

Fix: add `assets::Bounds(MeshData)` and `assets::Bounds(ModelData)` in Assets/Model.h. Have `FillItemPreview` read the cached bounds.

Verifier narrowed: four more sites do the min/max inside transform, skinning, de-interleave or rescale loops, so the helper does not fit them. No copy has drifted yet.

### C256. Button and item placement rules are copied between load, live add and remote add
*low - cleanup - effort M*

Doors spawn from their record through `SpawnDoor`, but buttons and items do not:
- `AddButton` (src/Game/DungeonWorld_Doors.cpp:545) repeats `LoadButtons`' kind and plate setup, including the hardcoded `"lever_plate"` (src/Game/DungeonWorld_Load.cpp:1520).
- The wall-scan rule appears in `AddButton`, `AddButtonRemote`, `MoveObject` and `EditCell`.

One copy has already drifted. `AddItemRemote` (Doors.cpp:656) counts niche items toward the four-per-cell limit, but `AddItem` (:591) does not. So placing an item on a browsed level can be refused where the same placement on the live level is accepted.

Fix: add a shared `CanPlace`/wall-scan helper, plus a `ResolveButtonMeshes` that takes the mount id from buttons.cat, and fix `AddItemRemote`. Leave `MoveObject`'s in-place moves alone: that design is deliberate.

### C258. Ray-sphere click picking is copied four times, and the niche pick ignores kUnit
*low - cleanup - effort S*

The ray-sphere click test is written four times: the door opener (src/Game/DungeonWorld_Doors.cpp:286), the sconce (src/Game/DungeonWorld_Fires.cpp:95), and the niche pick and drop (src/Game/DungeonWorld_Load.cpp:1581, :1625). They use three acceptance rules, but those agree whenever the eye is outside the sphere, so play behaves the same.

Two real defects:
- The niche pick's `p.y + 0.35f` and `0.4f * 0.4f` are raw metres, so the niche pick would not rescale with kUnit.
- The comment at Doors.cpp:276 says the door uses the niche items' test. It does not.

Fix: one ray-sphere helper beside `gfx::Camera::Ray`, with every radius given in units x kUnit.

### C259. Door motion is resolved twice, and DoorTypeAt scans records for no reason
*low - cleanup - effort S*

`SpawnDoor` (src/Game/DungeonWorld_Doors.cpp:94) and `SetDoorSettings` (:426) each resolve the ease, its overrides, the 0.7 default and the 0.05 guard. The guards already differ: spawn ignores a value at or below 0.05, settings clamps it. The inspector slider's 0.2 minimum hides the difference.

`DoorTypeAt` (:341) scans `m_entities`, and its comment says a Door does not keep its id. That is stale: `Door::type` exists (src/Game/DungeonWorld.h:2860).

Fix: add `ResolveDoorMotion` beside `ResolveDoorOpener`, and have `DoorTypeAt` return `door->type`.

### C260. Repeated literals and helpers: dimmed radius, flame colour, Trim, direction names
*low - cleanup - effort S*

- The dimming `(0.6f + 0.4f * brightness)` appears at src/Game/DungeonWorld_Flight.cpp:139, src/Game/DungeonWorld_Light.cpp:369 and src/Game/DungeonWorld_SpellLight.cpp:106, among others.
- The torch colour `{1, 0.62, 0.28}` appears in 6 places. Flight.cpp:95's ternary simplifies to `cargo->flameColor`.
- `Trim` is defined 5 times, and the copies already handle `'\r'` differently (src/Game/Style.cpp:12, src/Game/Serialize.cpp:15).
- Five north/east/south/west tables repeat `DirToken` (src/Game/Entity.h:41).

Fix: add a `DimmedRadius` helper, a `kTorchFlameColor` constant and one Core `Trim`, and use `DirToken`.

### C262. MessageLog's corner row draws its own buttons instead of using ui::Button
*low - cleanup - effort S*

The Log / Rest / Help row is a hand-made `CornerButton` struct. It does its own hit test, acts on press, draws its own skinned or flat face and centres its own label (src/Game/MessageLog.h:83, src/Game/MessageLog.cpp:73, :134, :227). Because of that it has no push, tooltip or carved text, and `uioverlap` cannot see the buttons.

Fix: make them ui::Button children placed in LayoutSelf.

Verifier caveats:
- Button needs an opacity or tint first, because the row fades with the footer.
- SpellbookPanel's MemberButton also skips ui::Button.
- In the flat look the footer fades its border, so it is not quite DrawPanelFace.

### C263. Effect icon and its time sliver are drawn by two separate copies
*low - cleanup - effort S*

The HUD strip (`EffectIcon::DrawSelf`, src/Game/CharacterPanel.cpp:112) and the sheet's Effects tab (`DrawEffectRow`, src/Game/CharacterSheet_Lists.cpp:474) each draw the icon and its time sliver. The two have already drifted apart: the inset is 1 px vs 2 px and the sliver 2 px vs 3 px. The HUD copy uses a flat `kSlotBg` rect, which CLAUDE.md says should always be `ui::DrawSlotFace`. The timeLeft/duration fraction is also written twice (CharacterPanel.cpp:125, CharacterSheet_Lists.cpp:264).

Fix: add one DrawEffectIcon helper and one remaining-fraction helper in PartyHudDraw, and call them from both places.

### C264. The press-then-release click logic is written out five times
*low - cleanup - effort S*

PortraitBox, EffectIcon, StatsArea and HandSlot each hand-write the same logic: set a pressed flag, then fire on a release over the widget. NameTag has a variant that resets when the button is no longer down (src/Game/CharacterPanel.cpp:37, :95, :170, :236; src/Game/HandSlot.cpp:25). StatsArea fires the same callback from two copies of it. The reset rules differ slightly, so sooner or later one widget will eat a release that the others honour.

Fix: add a small ui::ClickLatch in UI/Controls and use it in all five.

### C265. Easing re-implements EaseShape, and kEaseShapeCount is typed by hand
*low - cleanup - effort S*

Seven of Easing's 11 cases are EaseIn/EaseOut(EaseShape) written out again: EaseIn, EaseInCubic, EaseInQuart, EaseOut, Back, Elastic and Bounce. The verifier confirmed the algebra. The constants c1/c3/c4 are also declared twice (src/Core/Easing.h:66, :140).

`kEaseShapeCount = 9` (:200) is not tied to the enum or the name table. A tenth shape would silently vanish from EaseShapeFromName and from the DoorInspector dropdown.

Fix:
- Route those cases through EaseShape.
- Define `kEaseShapeCount = std::size(kEaseShapeNames)` with a static_assert.

### C266. Spell numbers are set twice, in the class constructors and in spells.cat
*low - cleanup - effort M*

These 15 spells are file pairs that hold nothing but a constructor:
- the 4 bolts
- the 4 wards
- the 4 sights
- Firelight, Skylight and Tidelight

Each spell's numbers are written twice. For example, src/Game/Spell/Firebolt.cpp:8 hardcodes power 14 / mana 8 / speed 8 / range 8. spells.cat [firebolt] sets the same values, and those override the constructor's (src/Game/Spells.cpp:145).

There are two ways to fix it:
- Move the number-only spells into a constexpr table in src/Game/Spell/AllSpells.cpp:40.
- Keep the classes and make their defaults the only source of the numbers.

The verifier narrowed the claim: these classes are where planned behaviour will go (src/Game/Spell/Waterbolt.h:5, Scrying.h), so they are not pure cost. Choosing between the two fixes is a matter of taste.

Against a documented decision: CLAUDE.md MAGIC says every spell is a CLASS, one file pair per spell.

### C267. A literal 4 is used instead of party::kMaxMembers, and a literal 5 for the sheet's tab count
*low - cleanup - effort S*

Nothing is broken today, because the cap is 4 and PartyRules enforces it.

The verifiers corrected one part of the finding. The monster threat arrays ARE covered: they are copied whole to and from the asserted SaveData array, so a size mismatch would not compile. CLAUDE.md's claim about them holds.

Real gaps:
- `m_throwCooldown`, `m_crackleClock` and `m_scorchClock` (src/Game/DungeonWorld.h:3401, :4289, :4293) use a literal 4, and their loops are guarded by the array size. A fifth member would silently get no throw wait, no scorch and no crackle.
- `ThreatAny` tests `threat[0..3]` by hand (:2414).
- `ResourceBarStyle::kMaxMembers` (src/Game/PartyHudTypes.h:39) duplicates party::kMaxMembers. More literal 4s: `PartyWindow::kMaxCards`, the `min(...,4)` in ControlBar and MemberCards, and MemberRow's loop.
- PartyWindow passes a literal 5 tabs and copies the tab-gap ratio (src/Game/PartyWindow.cpp:66), although `sheet::kModeCount` already exists.

Fix: size these with party::kMaxMembers, write ThreatAny as `any_of`, and use sheet::kModeCount.

### C268. The "first level, else level1" fallback is written out again in three places
*low - cleanup - effort S*

`DungeonWorld::FirstLevel` (src/Game/DungeonWorld.cpp:35) is a private static, so the same expression is written out again at src/Game/Game.cpp:1101 and :1122. It is also written out at src/Game/DungeonWorld_Move.cpp:457, inside a member that could simply call it. Dropping the "level1" fallback, for example, would mean finding all four copies.

Fix: move it to `Project::FirstLevel()` and call it from all four places. SaveGame.cpp:430's own level1 fallback is a different rule.

### C269. Small copy-paste blocks in GameUI
*low - cleanup - effort S*

- OnHandRightClick and OnHandMiddleClick have identical bodies (src/Game/GameUI_Items.cpp:129). The three OnPortrait* handlers differ only in the Mode they pass (src/Game/GameUI.cpp:313).
- RebuildForLanguage and RebuildForRoster end with the same HUD rebuild (:1313, :1328). The Clears at :1288, :1291 and :2258 do nothing, because the builders clear their own context.
- The centred subtitle draw and the 0.55 screen wash each appear three times (:2380).
- The content-sized tab stack is written three times, differing only in pad and gap (GameUI.cpp:115, GameUI_Stone.cpp:245, DialogLayout.cpp:111).
- Callers count MenuPanel's entries by hand (GameUI.cpp:358, :888), even though LayoutSelf already walks `m_list->Count()` (src/Game/MenuPanel.cpp:46). A miscount squashes every stone.

Fix: small shared helpers. Have MenuPanel take its count from its list.

### C270. The dev console repeats the same lookups, path builders and button drawing
*low - cleanup - effort M*

- The series lookup by (tid, node) is written five times: src/Game/DevConsole.cpp:262 and src/Game/DevConsole_Profile.cpp:382, :481, :813, :874. A helper of that kind already exists for the smoothing pool.
- The find-or-claim-a-slot block appears twice (Profile.cpp:347, :381).
- The "/"-joined zone path builder is copied word for word (Profile.cpp:1245, src/Game/DevConsole_Snapshots.cpp:116). The thread+path match appears three times in Snapshots.
- The button face is drawn four ways: DrawExpander (DevConsole.cpp:376), the Perf and Profile view toggles, and the Threads `button` lambda.

Fix: add FindSeries, ClaimSlot, BuildZonePath, SameRow and DrawButton helpers in DevConsole_Panel.h.

### C271. Console colours are retyped, which breaks "one colour per processor"
*low - cleanup - effort S*

DevConsole.h:350 defines `kCpuColor` and `kGpuColor` so that CPU and GPU colours agree by construction. The section files still retype RGB values:
- The "bound by CPU" verdict is drawn in the RAM gauge's amber (src/Game/DevConsole_Profile.cpp:603), right beside the blue cpu figure. The header says exactly this misreading was fixed.
- Every per-node graph, GPU zones included, uses a literal equal to kCpuColor (:858) instead of `kShareColor`.

The GPU verdict's purple is marked as deliberate at the site, but it still breaks the rule. The verifier found the FPS-green and Health-vs-Threads points weak: kGpuColor is also kAccent, and Health matches Threads' `!N ~M` column.

Fix: name the semantic colours in DevConsole_Panel.h and use the names everywhere.

### C272. Three copies of an empty Input, two of them function-local statics
*low - cleanup - effort S*

src/Game/Game.cpp:78 defines a namespace-scope `kNoInput`, with a comment warning that a function-local static is built on its first frame, which may be a guarded one. Even so, src/Game/Game.cpp:2506 declares its own function-local static in the same file, and src/Game/Game_Eval.cpp:97 another. Both are on unguarded paths, so nothing is violated today, but the next copy could land in a guarded frame.

Fix: one shared `kNoInput` or `NoInput()` in Platform/Input.h or Game.h.

### C273. setstat parses stat names by hand
*low - cleanup - effort S*

src/Game/Game_DevParty.cpp:1416 matches stat names with `starts_with`. So `setstat 0 strawberry 30` sets strength and `intx` sets intelligence, with no warning. This goes against Character.h:426, which says nothing else resolves a stat name by hand and provides `StatIndex` / `kStats`.

Fix: call `StatIndex(args[1])`, refuse on -1 and write through `c.*kStats[i].value`. Every spelling the scripts use is accepted by StatIndex.

### C274. The `rune` dev command is a weaker copy of `give`
*low - cleanup - effort S*

`rune` stows a tablet into `m_characters[0]` and never calls `OnItemFound`. `give` does call it, and its comment explains why a dev command that skips the hooks is a trap. `give rune_fire` already does the job, and nothing in the tools, evals or docs uses `rune`.

- src/Game/Game_DevParty.cpp:52
- src/Game/Game_DevParty.cpp:84

Fix: delete `rune`, or make it a one-line forward to `give`. Its summary says "lead member", but `give` also defaults to slot 0, so that wording is just stale.

### C275. The project.ini path is built by hand in 8 places
*low - cleanup - effort S*

Every other project file has an accessor (`CatalogPath`, `WorldMapPath`, `LevelMapPath`). The manifest has none, so `folder + "\\project.ini"` is typed out each time. A comment in catround records this path once losing a backslash without anyone noticing.

- src/Game/Project.cpp:104, 141, 231 (plus the fs::path form at :128)
- src/Game/Game_NewWorld.cpp:234, 278, 301
- src/Game/Game_DevWorld.cpp:612 and src/Game/Game_Editor.cpp:269

Fix: add `Project::ManifestPath()`, plus a static form that takes a folder, and use it everywhere.

### C276. World-map edit rules are repeated in Game lambdas and the console
*low - cleanup - effort S*

- `onEditArea` repeats `AddArea`'s extent and duplicate-id checks as a lambda in Game: src/Game/Game_World.cpp:709 against WorldMap.cpp:353.
- Location fields are written by hand twice, once in the dialog's `onEditLocation` (Game_World.cpp:734) and once in `worldloc set` (Game_DevWorld.cpp:947).
- The `worldsettings` comment at Game_DevWorld.cpp:1069 says both editors go through the same WorldMap calls. That is untrue.
- `MutableAreas()` has one caller (Game_DevWorld.cpp:982), and it only reads.

Fix: add `WorldMap::EditArea` and `EditLocation` and call them from both editors. Switch :982 to `Areas()` and delete `MutableAreas()`. The verifier noted that the `worldarea` comment at :998 is accurate.

### C277. Issue counting and printing is written three times
*low - cleanup - effort S*

`generate` and `validate` each count errors and print an "N error(s), M warning(s)" report, and ValidateDialog counts a third time.

- src/Game/Game_DevCommands.cpp:893 and :922
- src/Game/ValidateDialog.cpp:37

The two console formats differ: `generate` prints `@-1,-1` for an issue with no cell, and `validate` leaves the location out. Fix: one shared counter and printer. It must keep `validate`'s line shape, because LevelBuildTest.py:336-342 parses those lines.

The finder also flagged `dungeons rename`. The verifier dropped that point: it already forwards to the same `RenameType` call in one line.

### C278. Dead ward helpers, `Inventory::Grow` and other unused members
*low - cleanup - effort S*

No caller exists anywhere for:

- the two-argument `RemoveEffect`, both `FindWard` overloads, `RemoveWard` and `HasShield` (src/Game/Character.h:310-338)
- `kEquipLabels` and `Inventory::Grow` (src/Game/Inventory.h:237, :323)
- `Party::SetMoveEasing` / `SetTurnEasing` (src/Game/Party.h:111)

The comment at Character.h:321 says these "remain for the UI and for tests". Nothing in either uses them.

Fix: delete them all, but keep the one-argument `RemoveEffect`, which `TickSupplies` uses. Also fix the stale comments at Inventory.h:33 and docs/movement.md:31.

### C279. Ward effects have no school, so the `effect` dev command hard-codes one
*low - cleanup - effort S*

The ward constructors never set `m_school`, so every ward's `DefaultSchool()` falls back to Fire. To cover for that, the `effect` command keeps its own id-to-school table (src/Game/Game_DevParty.cpp:466). Its `ahead` path uses `DefaultSchool()` instead (src/Game/DungeonWorld_Combat.cpp:1313), so `effect stoneskin ahead` lands a fire-school stone skin. The hard-coded table also gives `starving` and `parched` Fire, although their own kinds say Earth and Water. Magnitude and seconds are parsed twice (:432 and :460).

Fix:

- Set `m_school` in each ward constructor (src/Game/Effect/WardEffect.cpp:20), as DotEffect and SupplyEffect already do.
- Use `DefaultSchool()` in both paths and delete the table.
- Parse magnitude and seconds once.

### C280. Dead DungeonWorld methods and `Ledger::DropAll`
*low - cleanup - effort S*

No callers exist for:

- `MonsterInstanceAt` (src/Game/DungeonWorld.cpp:1526)
- `MonsterPowerRange` (src/Game/DungeonWorld_Census.cpp:57)
- `ThrowCooldown`, `Parked()` and `MultiMaterialModel::GroundOffsetY` (DungeonWorld.h:1029, :1298, :2533)
- `Ledger::DropAll` (src/Game/DamageLedger.cpp:114)

DropAll's comment says level loads and new games use it. In fact every reset path calls `RebaseDamageLedger`.

About 15 more public methods are called only from inside DungeonWorld's own files, for example `PickItemIndex` and `NicheOpenAt`. Fix: delete the dead ones, and make the internal ones private or move them as subsystems are extracted.

### C281. Seven Core functions have no callers, and one is a trap
*low - cleanup - effort S*

These have no callers in src/ or tools/:

- `alloc::ResetThisThread` (src/Core/AllocTrack.cpp:177). If anyone called it mid-frame, EndFrame's unsigned subtraction would wrap and report a huge false violation.
- `diag::FindThread` (src/Core/Diagnostics.cpp:313)
- `stack::Init` and `stack::Capture` (src/Core/StackTrace.cpp:70, 75)
- `Timer::TotalSeconds` (src/Core/Time.h:17). It sums clamped dt, so a first caller would get a wrong total.
- `EaseShapeName` (src/Core/Easing.h:202)

Also, the `open` array in DumpTrace (src/Core/Profile.cpp:456) is written but never read. Fix: delete all of these, and make `crash::DumpsWritten` file-local.

### C282. Unused public API in Assets, Platform, Audio and Animation
*low - cleanup - effort S*

- `LoadGltf` / `LoadObj` are exported although the header calls them internal; only `LoadModel` uses them (src/Assets/Model.h:184).
- No callers for `Input::WasKeyReleased`, `AudioEngine::IsAvailable` / `MasterVolume` or `Animator::Fading` (src/Platform/Input.h:43, src/Audio/AudioEngine.h:39 and :60, src/Animation/Animator.h:66).
- `ImageCache::data` is set but never read (src/Assets/Model.cpp:55).

Fix: move the two loaders into an internal header (LoadObj lives in ObjLoader.cpp, so an anonymous namespace will not work) and delete the rest. Removing `WasKeyReleased` also leaves `m_keysReleased` dead.

### C283. `ui::TextOutput` is dead but still compiled and documented
*low - cleanup - effort S*

MessageLog replaced it and nothing creates one (src/UI/Controls.h:107, src/UI/Controls.cpp:255). It also models two patterns the project has ruled out:

- one heap string per line in a `std::deque` (see docs/message-allocation.md)
- its own wheel scroll, where CLAUDE.md says ScrollArea owns all scrolling

Fix: delete it and the `<deque>` include (Controls.h:30). Remove its mentions in Controls.h:7 and :1019, src/Game/MessageLog.h:24, docs/ARCHITECTURE.md:42 and docs/ui-hierarchy.md:264.

### C284. Unreachable `optionsFor` cases, a dead pool lister and unused dialog helpers
*low - cleanup - effort S*

TypeEditorDialog shows TextureSet and Model fields as asset-picker buttons, so these never run:

- the `optionsFor` cases at src/Game/Game_Wiring.cpp:471-472
- `InstalledTextureSets` (src/Game/AssetUtil.cpp:209)

Also unused:

- `DialogTitleFont` / `DialogTextFont` (src/UI/Controls.h:1111, :1116)
- `kLabelW` / `kFieldX` / `kFieldW` (src/Game/TypeEditorDialog.cpp:29)
- `PreviewRect`'s two arguments, ignored in four dialogs
- the `InstanceInspector::FormRow` forwarding shim (src/Game/InstanceInspector.cpp:56)

CLAUDE.md names the two font helpers as the convention for raw draws, so adopting them fits the docs better than deleting them.

The verifier narrowed the `levelcheck` point. Sidecar stems only inflate the `installed_models=` count, by 76; the check does not wrongly pass today.

### C285. The old `poison` / `bleed` / `element_dot` on-hit aliases have no users
*low - cleanup - effort S*

ParseOnHit still reads these old fields (src/Game/DungeonWorld_Load.cpp:540-548). No .cat file in any project, template or the library uses them, and CatalogSchema offers none.

Fix: delete the alias branches and their header comments (DungeonWorld.h:2621, :3665), keeping only `on_hit`. Then update the comment at line 37 of each effects.cat, CLAUDE.md and docs/effects.md.

Against a documented decision: CLAUDE.md says these "still load as aliases". They were kept for catalogs that had not been rewritten yet, and none of those are left.

### C286. FireEffect::SteadyCount is unused and the reserve comments are stale
*low - cleanup - effort S*

`FireEffect::SteadyCount()` has no callers (src/Game/FireEffect.h:44). `ReserveParticleScratch` now sizes from hard ceilings (`Capacity()`, `CapacityFor`), but it still adds `peak / 4` described as "headroom over the mean" (src/Game/DungeonWorld.cpp:579). That describes the old sizing model. Delete `SteadyCount` and keep `SteadyCountFor`. Then either drop the extra quarter or document it as slack for the item list, which can grow past `kReservedItems`.

### C287. Dead functions, constants and fields in the HUD and character sheet
*low - cleanup - effort S*

All of these are dead, and the verifier confirmed every one:
- `DrawResourceBar` has no callers (src/Game/PartyHudDraw.cpp:86).
- `CharacterSheet::BakeStats()` is an empty stub (src/Game/CharacterSheet_Stats.cpp:30).
- Six layout constants left over from before ScrollArea, such as `kWheelStepRem` and `kScrollThumbMinRem` (src/Game/CharacterSheetLayout.h:61-158).
- `kDefBad` is unused, and its literal is repeated at src/Game/CharacterSheet_Inventory.cpp:293.
- `Row::higherBetter` is never false.
- `SheetList::Measure` ignores its Font argument.
- `SpellbookPanel::LayoutSelf` overwrites `MemberRow`'s ctor layout every frame.
- `DrawRuneFace`'s `background` path is unreachable (src/Game/PartyHudDraw.cpp:388).

Delete them. The dead constants look like live tuning knobs.

### C288. Small dead or duplicated bits in MapView
*low - cleanup - effort S*

- `MapView::m_device` is set and never read (src/Game/MapView.h:448). The ctor's `device` parameter is still used.
- src/Game/WorldMapView.h:26 includes GameSettings.h but never uses it.
- A local `kPi` shadows Core/MathTypes.h:29 (src/Game/MapView.cpp:34). The same happens in FloatingPanel, DungeonWorld and MonsterAI.
- Raw `0x10/0x11/0x12` modifier keys appear at src/Game/MapView_Tools.cpp:133.
- The 6x6 region minimum is stated twice: src/Game/MapView_Tools.cpp:280 and src/Game/MapEditor_Shapes.cpp:66.
- A forward declaration is redundant after the include.

Fix: name the modifier keys once in Platform/Input.h and add one `MapEditor::kMinRegion`.

### C289. Dev console hit-scale safeguard does nothing; ProfSeries has write-only fields
*low - cleanup - effort S*

Nothing writes `m_renderW/H`, so `hitScale` at src/Game/DevConsole.cpp:241 is always 1. The comment above it describes protection that does not exist. Every editor dialog has the same window-vs-device mismatch, so the console is no worse than the rest of the UI. Separately, `ProfSeries::name/thread/depth` are copied every frame in profiling builds (src/Game/DevConsole_Profile.cpp:399), and nothing reads them. Fix: either store the size in Render or delete the fields and the comment, and drop the three ProfSeries fields.

### C290. The stale dev `preview` command keeps a render branch and four Game members alive
*low - cleanup - effort S*

The `preview` command only tries `<name>.gltf` (src/Game/Game_DevCommands.cpp:1024), so committed .glb models such as `rock` report "no model". It also indexes `meshes[0]` without checking for an empty list. The Game.h:1110 comment ("for now") is stale, and no harness uses the command. It keeps four Game members, an orbit tick and two render branches alive (src/Game/Game.cpp:2739, 2913).

The verifier narrowed the finding:
- A typo cannot abort the process. Only a file that exists but fails to parse asserts.
- AssetDialog (the only real twin) already handles .glb and empty meshes correctly.

Fix: delete the command and its plumbing. `assetpicker models` previews pool models correctly.

### C291. Dead DungeonMap API and copy-only shims
*low - cleanup - effort S*

- `NicheWallsAt`, `AddNicheRecord`, `AddFeatureRecord`, `AddBoreRecord` and `ThemeMemberIds` have no callers (src/Game/DungeonMap.h:491-680). The parser pushes records directly.
- The `FixtureTypes fixtures = {}` default is never used.
- `SurfaceVariantFor` is called only by `ResolveSurfaceVariant`, so the include comment at MapView.cpp:20 is stale.
- `FromText` copies text into a vector just to suit `ReadLevelLines` (src/Game/DungeonMap.cpp:35).
- `Catalog::Serialize` deep-copies each entry. `Load` already moves, so only this rare save path copies.

Fix: delete the dead members and the default, make `SurfaceVariantFor` file-local, and have `ReadLevelLines` take `string_view`.

## 9. Data, saves and editor integrity

### C292. Level reset is copied by hand into several paths, so blasts, smashed props and DoTs carry across a level change, new game or load
*high - bug - effort M*

Each reset path lists by hand what it clears, and the lists no longer match.

- **Blasts:** `m_activeBlasts` is cleared only in ResetForEval (src/Game/DungeonWorld_Save.cpp:150). BeginLevelLoad (src/Game/DungeonWorld_LevelIO.cpp:81), ResetForNewGame (Save.cpp:85, which LoadGame also uses), BuildArena and InstallLevel (src/Game/DungeonWorld_Validate.cpp:132) all skip it. A poison flask's 8 s gas keeps biting the same coordinates on the next floor, in a loaded save or in a new game. Stair pairs share a square, so the party can arrive inside the cloud.
- **Same-level new game or load:** ResetForNewGame never resets breakables, monster `effects` or the pit fall. A door smashed after the save comes back shut and wrecked: it blocks movement and can't be opened or broken again. Burning monsters keep burning. `m_fixtureBreaks` also carries over to the next level by coordinates.

**Fix:** add one `ClearLevelTransients()` and call it from every reset site (after StashActive in BeginLevelLoad). Move ResetForEval's breakable, effects and fall resets into ResetForNewGame. Empty pooled containers with `clear()`, not `= {}`.

The verifiers found no crash. The gas window is about 8-12 s.

### C296. The item on the cursor survives Start New Game and the eval reset
*high - bug - effort S*

Only UnloadWorld resets `m_heldItem` (src/Game/Game.cpp:617), and LoadGame overwrites it. StartNewGame (src/Game/Game.cpp:1070), ReturnToTitle (Game.cpp:1536) and the recycle path of Game::ResetForEval (src/Game/Game_Eval.cpp:63) don't touch it.

**Repro:**
1. Lift a floor dagger onto the cursor.
2. Esc, then Return to Main Menu (a party wipe works too).
3. Start New Game in the same world.

The new party holds the dagger. ResetForNewGame also puts authored floor items back, so the dagger is on its square too: the item is duplicated. A lit torch on the cursor keeps lighting the new party.

**Fix:** clear the cursor in one helper called from both StartNewGame and ResetForEval. ReturnToTitle alone is not enough. Extend `Eval.ps1 -SelfTest` to print the cursor after putting an item on it.

The harness side is latent: no suite runs `torch take` today.

### C299. After leaving a random encounter, every save on the world map is refused
*high - bug - effort M*

`InEncounter()` only compares CurrentLevel with "~encounter" and ignores `onWorldMap` (src/Game/Game_Generate.cpp:150). For an encounter, LeaveDungeon skips ParkActive, and SetOnWorldMap never changes the active level (src/Game/Game_World.cpp:342). So after the party wins an ambush and takes the exit stair, the encounter is still the current level. SaveGame refuses with world.nosave (src/Game/Game.cpp:1151) until the party enters a dungeon. Encounters are on by default.

Relaxing the guard alone is not enough:
- The save would write `currentLevel = "~encounter"` (src/Game/DungeonWorld_Save.cpp:594), and LoadGame would assert on a missing file.
- An empty stem would reach `BeginLevelTransition("")`.

**Fix:** refuse only when `InEncounter() && !onWorldMap`. On the world map, write a real stem: the last parked level, else the project's first level. Add an eval that runs `encounter`, `leave`, `save`, `load`.

The bug can be recovered from in play and corrupts nothing, so medium is defensible.

### C301. Each loader hard-codes the model file extension, so an editor-imported item or a picked model aborts the game
*high - bug - effort M*

A catalog `model` is a bare name, and the picker lists both .gltf and .glb. Each loader then appends its own extension:
- items use `.glb` (src/Game/DungeonWorld_Load.cpp:1412);
- decorations, monsters, fixtures and features use `.gltf` (Load.cpp:1977, :611, :2020, :311).

The editor's import always writes `<name>.gltf` (tools/AssetBaker/ModelImport.cpp:172). So Weapons "+ New..." then Import writes sword.gltf, and the next load asks for sword.glb and aborts in LoadModelOrDie. PreloadItemKinds builds every item at world load (Load.cpp:1690), so every launch aborts until the .cat is edited by hand or reverted. Picking a .glb for a decoration, monster or fixture fails the same way in reverse. `levelcheck` only compares names, so it passes the broken entry.

Even with the right extension, an imported item renders white, because ItemKindFor never binds the entry's `texture`.

**Fix:** add one `ResolveModelFile(name)` in AssetUtil and use it in every loader, `levelcheck` and the previews. Choose single-mesh vs multi-material from the file's contents, which also retires `multimaterial`. Keep the documented load-or-die. Instead, have the create dialog and the type editor's Save refuse a model that category can't load.

### C310. The bore (window) brush on a browsed level writes to the active level
*high - bug - effort S*

In ApplyBrush, every placement checks `remote = m_view.Browsing()` except the bore branch. It calls `m_world->AddBore(...)` on the live map, with the comment "active level only for now" (src/Game/MapEditor.cpp:832). The face and the placement check, though, come from the browsed map (src/Game/MapView.cpp:467).

Usually the user just gets a misleading "Can't place". When the active level has a suitable wall at the same coordinates, it silently gains a window and `savemap` writes it, while the browsed view shows no change. EraseRemote (src/Game/DungeonWorld_Remote.cpp:364) can't remove a bore either.

**Fix:**
- Add `AddBoreRemote(stem, ...)` as `EnsureMapStash(stem).AddBore(...)` and route the bore branch through `remote`.
- Add `RemoveBoreAt` to EraseRemote.
- Delete the unused 3-argument AddBore overloads (src/Game/DungeonWorld_Editing.cpp:560).

It is editor-only and undoable, so medium is defensible.

### C311. RespawnFromRecords rebuilds the level from stale records, so type saves, renames and undo lose or bring back editor work
*high - bug - effort M*

On the active level, the live lists are the truth for monsters and decorations. Placing one adds only a live instance, and erasing one leaves its record behind. RespawnFromRecords (src/Game/DungeonWorld_Undo.cpp:135) rebuilds from those stale records. Two callers run it without protecting live state:
- the type editor's Save, for every category except surfaces, themes, lights and trails (effects, flags and items included; src/Game/Game_Wiring.cpp:572);
- RenameType (src/Game/Game_Editor.cpp:964), which also clears undo.

The result:
- editor-placed monsters and decorations vanish, and erased ones come back;
- killed monsters revive, and picked-up items reappear on the floor;
- doors reset and inspector overrides are lost, and the next `savemap` writes all of this.

Undo after erasing an authored monster brings it back. SweepTypeRefs counts only records, so a delete can leave a live placement naming a deleted type.

**Fix:** in those two callers (not inside RespawnFromRecords, which would break undo), sync the records, then StashActive, respawn, and ApplyActiveSnapshot. Respawn only for categories with a kind cache. The erase paths should remove the record too.

Against a documented decision: CLAUDE.md says "Placement appends to the live world lists". The long-term fix is to make placement record-first, as doors and items already are.

### C326. Saving a level writes each monster at its current square, not its spawn
*high - bug - effort S*

ActiveEntText writes `mon.x, mon.z`, the live square (src/Game/DungeonWorld_LevelIO.cpp:363), while the same record's facing and leash check use the spawn. Everything else treats spawnX/spawnZ as the .ent square: the editor's move tool (src/Game/DungeonWorld_Move.cpp:155), new game and the save diffs.

The editor is a live view, so patrolling monsters, chasing monsters and corpses all move. Every inspector Save (src/Game/Game_Wiring.cpp:930 and the others beside it) and every `savemap` then silently moves their authored spawns, straight into the git tree. In-memory spawnX is not updated, so a new game in the same session still uses the old square. The title screen's Editor entry opens paused, which only partly helps.

**Fix:** write `mon.spawnX, mon.spawnZ`. Optionally add a `LiveMonsterRecords()` twin of LiveDecorationRecords, so the live-to-record mapping lives in one place.

### C333. Random encounters hard-code the exit stair as "stairs_exit", and exit placement is written four ways
*high - bug - effort S*

StartEncounter writes `stairs stairs_exit ... north` (src/Game/Game_Generate.cpp:206). Every other path finds the exit by its `exit = 1` flag:
- the wizard (Game_Generate.cpp:389);
- ExitStairType (src/Game/Game_NewWorld.cpp:126);
- the exit list (src/Game/DungeonWorld_Levels.cpp:80).

The type editor's rename updates level records but not this literal. After a rename, the next ambush installs an unknown stair type. DecorationKindFor falls back to `stairs_exit.gltf`, which does not exist, and the game aborts mid-play (src/Game/DungeonWorld_Load.cpp:2140). A cached kind can hide this until the next launch. In the shipped world the delete is refused, because the crypt levels use this stair, so a rename is the realistic trigger. Facing north can also leave the arriving party facing rock, which the wizard avoids.

**Fix:**
- Move ExitStairType into Project and use it everywhere.
- Have StartEncounter refuse, with a log line, when a world has no exit stair.
- Use one helper that picks the open side, with an option for copy-level's exit placed beside the start.
- Have DeleteType refuse to delete the last exit stair type.

### C334. The level generator hard-codes the door type "wooden_door", so renaming or deleting that type crashes the game
*high - bug - effort S*

The generator writes every lock as `door.type = "wooden_door"` (src/Game/Generate.cpp:699), and no caller ever changes it. If you rename or delete Wooden Door in the type editor, the rename sweep fixes level records but not this literal. In a fresh world no level uses the type, so the delete is not even refused.

Create, Regenerate or the wizard (the default is `locks:1`) then writes `door wooden_door ...`. When you enter that level, SpawnDoor (src/Game/DungeonWorld_Doors.cpp:64) looks for `wooden_door.gltf`, LoadModelOrDie aborts, and any unsaved stash edits are lost.

Fix:
- Have the caller pass the lock door type in Params, found the way ExitStairType finds the exit stair (src/Game/Game_NewWorld.cpp:125): an entry marked `lock = 1`, else the first non-hidden door that has an `opener`. "Non-hidden" alone is not enough, because the portcullis has no opener and would lock the party out. If nothing qualifies, place no locks.
- Fix the hard-coded `stairs_exit` the same way (src/Game/Game_Generate.cpp:206).
- Fix SpawnDoor's stale comment at lines 66-69.

Do not add a Contains() guard to SpawnDoor. CLAUDE.md says a record naming a missing type is not a soft failure at load.

### C344. Opening a wall on a browsed level leaves wall decorations floating, and the saved level can no longer be loaded
*high - bug - effort S*

To reach it:
1. Browse to another level.
2. Open a wall square a banner hangs on. Any brush does it, including a shape brush carving through rock (src/Game/MapEditor_Shapes.cpp:129).

The remote prune only re-faces Button records (src/Game/DungeonWorld_Remote.cpp:409-428). The "soft loader" that comment and src/Game/DungeonWorld.h:4610 rely on does not exist.

So the banner floats, and `savemap` writes its `wall=` record unchanged. From then on, every parse of that .map hits the DN_ASSERT at src/Game/DungeonMap.cpp:410, which aborts in every build. That covers entering the level, browsing it, and live validation or the census. The game still boots unless this is the start level, and `git checkout` gets the file back.

The live path already handles this correctly (src/Game/DungeonWorld_Editing.cpp:94-109). Fix: add one DungeonMap helper, next to FreeSconceWall and FreeNicheWall, that moves the record to another solid wall of its cell or drops it. Call it from EditCellRemote, have the live path use the same rule, and fix both comments. A skip-and-warn in the parser is an optional extra safety net.

### C345. Editing terrain glyphs, adding terrains or deleting one can make the world impossible to open
*high - bug - effort S*

WorldMap::Load asserts on three things: a lowercase glyph, two terrains sharing a glyph, and a grid glyph that no terrain owns (src/Game/WorldMap.cpp:150-157, 185). It runs on every world open, before the editor exists (src/Game/Game.cpp:529). Nothing stops bad data on the way in:
- "+ New..." creates a terrain with no glyph, which reads as '?' (src/Game/Game_World.cpp:38). Two new terrains therefore clash.
- The type editor saves any glyph text without checking it.
- Deleting a terrain is never refused. SweepTypeRefs returns early (src/Game/DungeonWorld_Editing.cpp:275), and SweepCatalogRefs skips terrain (src/Game/Game_Editor.cpp:911). The documented reason for skipping terrain only covers renaming its id.

The running session keeps its own copy of the terrains, so nothing looks wrong until the next launch aborts. The only recovery is fixing terrain.cat by hand.

Fix:
- On save, require exactly one character, not lowercase, and unique.
- Give a new terrain an unused glyph.
- On save, sync m_worldMap's terrains by id. The grid stores indices, so a glyph change then rewrites the grid for free.
- Refuse a delete while any cell uses the terrain.
- Add a CheckWorld rule.

Do not make Load remap unknown glyphs. That silently repaints cells, and SaveWorld would then write the repainted grid back to disk.

### C351. The editor has not been able to place an item into a niche since the placement check was added
*high - bug - effort M*

Since 81bdc8fa (2026-08-11), every brush checks its placement first. Items use Mount::FloorSlot, which refuses any square that is not walkable (src/Game/Placement.cpp:93). The niche branch (src/Game/MapEditor.cpp:852) needs a solid square, because NicheFacesAt returns nothing on floor (src/Game/DungeonWorld_Doors.cpp:478).

The two conditions can never both hold, so AddNicheItem (only called from there) never runs, and no test covers it. Even before the check, the branch bypassed the single placement resolver, always took `faces[0]`, and the preview never showed the pocket.

It only affects the editor. Hand-written `niche=` items still load, and players can still drop items into niches. The verifier puts it at the low end of high.

Fix:
- Make "into a niche" a Placement result: a FloorSlot brush over a solid square, with a picked face where NicheAt is true.
- Track the hover face for item brushes too (src/Game/MapView.cpp:552).
- Have ApplyBrush call AddNicheItem from that result.
- On a browsed level, either refuse or add an AddNicheItemRemote.
- Delete the dead branch, and add an EditorTest case that checks the .ent gains `niche=`.

### C366. The title menu never shows Continue or Load after a first save if the player pauses again
*high - bug - effort S*

BuildMenuList and BuildPauseMenu write the same `m_menuHasSaves` flag (src/Game/GameUI.cpp:355, 887). After the first save, the game returns straight to play, so the "saves changed" flag sits unread. Then:
1. The next Esc rebuilds the pause list (src/Game/Game.cpp:2539), which sets the shared flag to true.
2. RefreshMenuEntriesIfDirty sees no change, clears the dirty flag, and never rebuilds the title list (src/Game/GameUI.cpp:2103).
3. ReturnToTitle only resets the page (src/Game/Game.cpp:1546).

So save, Esc, Return to Main Menu shows no Continue or Load until the next launch. This is exactly the bug the comment at GameUI.cpp:2089 says the mechanism was added to fix. The save itself is fine.

Fix: keep one flag per list and compare each separately (or rebuild both lists whenever the dirty flag is set). Also rebuild the title list in ReturnToTitle as a backstop.

### C293. Smashed fixtures stay smashed on another level, after loading a save, or in a new game
*medium - bug - effort S*

SeedFixtureBreakables copies broken state, hp and burn effects from the old table by (x, z, wall, type), with no level check (src/Game/DungeonWorld_Combat.cpp:2538). BeginLevelLoad and InstallLevel never clear m_fixtureBreaks (src/Game/DungeonWorld_LevelIO.cpp:72). So a sconce on the same cell of a stacked stairwell arrives broken and burning, and is saved that way.

The verifier found a more common path. ResetForNewGame (src/Game/DungeonWorld_Save.cpp:58), used by LoadGame and by a new game on the same level, never resets broken fixtures, decorations or doors. Loading an earlier save leaves them smashed.

Fix: move ResetForEval's reset lines (Save.cpp:155-161) into ResetForNewGame, and clear m_fixtureBreaks in BeginLevelLoad and InstallLevel.

### C295. Resting continues into a loaded game
*medium - bug - effort S*

Rest is documented as not saved, but neither ResetForNewGame (src/Game/DungeonWorld_Save.cpp:58) nor Game::LoadGame (src/Game/Game.cpp:1254) ends it. RestTimeScale (src/Game/DungeonWorld.h:548) keeps multiplying `wdt` (src/Game/Game.cpp:1920), and lockstep AI stays on.

If you load while resting, the loaded game runs at 60x until the first blow. If the party is at full strength, rest stops at once but logs a stray "recovered" line in the new game.

Fix: end rest quietly in ResetForNewGame: restore lockstep from m_restLockstep, then clear m_resting and m_restEndReason. ResetForEval clears m_resting but does not restore lockstep, and the same fix covers it.

### C297. Undo history survives a new game, load or eval reset on the same level
*medium - bug - effort S*

Undo history is only cleared by a level change, a level or type rename, or a dungeon delete. After a same-level load, new game or `reset`, Ctrl+Z in the editor restores the old session's snapshot (src/Game/DungeonWorld_Undo.cpp:104-105). That brings back its fog, dead monsters, collected items and door states. In an encounter, undo would put crypt1's map under "~encounter". EditorTest phases that `reset` also inherit earlier phases' undo stacks.

Fix: call ClearUndoHistory in ResetForNewGame, and in InstallLevelFromText (or StartEncounter) before m_currentLevel is overwritten. Do not put it in InstallLevel: the stems already match there (src/Game/DungeonWorld_Validate.cpp:94), and regenerate deliberately wraps InstallLevel in an undo step.

### C298. Loading a save on another level silently throws away unsaved editor edits
*medium - bug - effort S*

BeginLevelLoad only stashes the active level when `stashCurrent` is true, and it gates the dynamic state, the static map and the .ent records together (src/Game/DungeonWorld_LevelIO.cpp:37-43). Two paths pass false: LoadGame onto another level (src/Game/Game.cpp:1448) and a new game that opens elsewhere (src/Game/Game.cpp:1061). Walls painted on the active level without `savemap` are then dropped, with no prompt and no log line.

Levels edited remotely keep their stashes, so this loss looks accidental. It also breaks CLAUDE.md's statement that the map is stashed on every swap so unsaved edits survive.

Fix: always run StashStaticMap (it already resets play-time changes on its copy) and stash the .ent records if they are dirty. Gate only StashActive on stashCurrent.

### C300. Eval `reset` reloads the current level instead of the eval level, and crashes after an encounter
*medium - bug - effort M*

DungeonWorld::ResetForEval re-reads `m_currentLevel` (src/Game/DungeonWorld_Save.cpp:134). Game::ResetForEval (src/Game/Game_Eval.cpp:63) never switches to `evalLevel`, where a harness new game starts. Consequences:
- After `encounter ... leave`, the level is "~encounter", so the next `reset` hits the DN_ASSERT in the DungeonMap file constructor (src/Game/DungeonMap.cpp:17).
- After levelplay.eval, the next batched suite runs on crypt1, so it no longer matches a solo run.

Eval.ps1's batches never change level, so they don't hit this. But worldpersist.eval:59 already resets while on crypt_gate, so its "loaded over another level" case probably tests the same-level branch instead.

Fix:
- Reload evalLevel and clear the stashes and undo history.
- Refuse, with a log line, when the stem starts with '~'.
- Add a batched pair of suites that changes level to Eval.ps1 -SelfTest.

### C302. One prop cache is keyed by bare id across four catalogs, and saving an item clears the wrong cache
*medium - bug - effort M*

Two related cache bugs:

- **Shared cache.** DecorationKindFor (src/Game/DungeonWorld_Load.cpp:1929) caches decorations, doors, buttons and stairs by bare id. It reads the catalog only on a cache miss, and the cache lasts across levels. `[portcullis]` is in both decorations.cat and doors.cat in every project and the template. Whichever is used second draws with the other's mesh, texture and culling, and the two share a map icon and map-arrow flag (src/Game/DungeonWorld.cpp:1763). Nothing places either yet, and the damage is visual only.
- **Wrong cache on item save.** ReloadTypeKind (src/Game/DungeonWorld_Editing.cpp:223-225) clears the decoration cache for items, weapons and armor, and never refreshes m_itemKinds. A saved weapon edit (damage, model, procs) has no effect until the world reloads. Saving `khukri` also clears the khukri decoration and its .glb, so its textures get loaded twice.

Fix:
- Key the cache by (catalog, id) in every lookup by id.
- For now, rename the decoration to `portcullis_grate`, and check for ids used in a related catalog on create, rename and duplicate.
- Rebuild an ItemKind in place rather than erasing it, because thrown items and the icon bank hold pointers to it.

### C303. Catalog field defaults are written in several places and disagree
*medium - bug - effort M*

The type editor shows the schema's `def` for a missing field and copies it into new types, but the loaders use their own fallbacks:
- **Monsters:** hp 10 vs 12, damage 3 vs 4, attackcd 1.5 vs 1.6, movecd 0.5 vs 0.6 (src/Game/CatalogSchema.cpp:247, src/Game/DungeonWorld_Load.cpp:639, src/Game/Threat.cpp:58-70).
- **Others:** item holdable and weight, decoration solid, ceiling height_scale.

The monster mismatch causes no problems yet, because all 16 shipped monsters set every field. The visible bug: six bags and herbs in items.cat show Holdable ticked while the game treats them as not holdable, and duplicating [backpack] creates a holdable copy.

Threat.cpp also has its own copies of game rules:
- It hard-codes slash/pierce/bash instead of using the `physical` flag.
- It uses its own 0.05/0.95 clamp instead of Balance's crit and fumble thresholds.
- ThreatProfile copies MonsterAttack's formula.

MonsterKindFor and ItemKindFor also parse the same strike traits separately. Actual blows still go through fx::Deal, so damage itself is unaffected.

Fix:
- Keep `def` as the value a missing field means, and add a separate starting value for new entries.
- Have every reader take the missing-field value from the schema table.
- Decide what a missing holdable or solid should mean.
- Share one stats and strike-traits parser between monsters and items.
- Delete the unreachable threat::FromCatalog path, and fix the stale `offense` help text.

### C304. The rename/delete reference sweep is a hand-kept list that misses lights, trails, door parts, flags and item links
*medium - bug - effort M*

The comment on `Game::SweepCatalogRefs` says the list is closed, but it has fallen behind the schema. It never sweeps:
- `light` (fixtures, items, weapons, armor, effects) and `trail`
- door `trim`/`frame`/`opener`/`mount`, and flags `dungeon`
- fields with no schema row: the `lit_as`/`spent_as`/`drink_as` family, spells.cat `light`/`trail`, `start_items`

Lights and Trails are palette categories, so the type editor can delete `torch` or `fire_sconce` while fixtures and items still name it. Most of these fail softly (fallback light plus a load warning). The sharp case: rename `torch_lit` and torches silently stop lighting.

- src/Game/Game_Editor.cpp:754, :1021
- src/Game/CatalogSchema.cpp:218
- src/Game/DungeonWorld_Load.cpp:1296

Fix: build the generic sweep from schema CatalogRef rows. Add rows for spells and the `*_as` links. Refuse to delete light ids the code names.

Against a documented decision: CLAUDE.md calls this sweep "a closed list". It has drifted.

### C305. Surface features are invisible to the type rename/delete sweep
*medium - bug - effort S*

`TypeRecords` has no family for floor and ceiling features: the WallFeature case sweeps only niches and bores. `DungeonWorld::SweepTypeRefs` has no "surfacefeatures" branch. So a delete is never refused, and a rename leaves records naming the old id. Those resolve to no mesh, and a plain block is drawn instead. RenameType also flags geometry only for wallfeatures.

The category is in the palette, so this is reachable, but it is latent: no shipped level has a feature record yet.

- src/Game/DungeonMap.cpp:1320
- src/Game/DungeonWorld_Editing.cpp:255
- src/Game/Game_Editor.cpp:1013

Fix: add `TypeRecords::SurfaceFeature` over `m_features`, map the key to it, and pass geometryToo on rename.

### C306. Effects can be renamed even though delete is refused, and the rename drops their tuning
*medium - bug - effort S*

DeleteType refuses `effects` because they are class-backed. RenameType has no such guard, and the type editor's title is always a rename control. Renaming [burn] from the Balance dialog appears to work. EffectBook::Build then ignores the renamed entry with a warning, and the burn class falls back to its constructor defaults. It loses its name, icon, plume, light, damage type and stacking, while every `on_hit = burn` still resolves to the class.

Attacks and spells, whose identity is also C++, are not delete-guarded either (console `typeset` only).

- src/Game/Game_Editor.cpp:1000, :938
- src/Game/TypeEditorDialog.cpp:296
- src/Game/Effect/Effect.cpp:306

Fix: add a per-catalog "identity is C++" flag (effects, attacks, spells) that refuses both rename and delete.

### C327. DungeonEntities::Add can reuse a removed record's id
*medium - bug - effort S*

Add gives a new record max(id)+1. Removing the highest-id record therefore frees its id for the next placement. That breaks the invariant DungeonWorld.h:4567 relies on ("record ids are stable across removals"). EraseRemote never clears the removed id's entry in `m_levelStates`, and ApplyActiveSnapshot applies collected, hp and position diffs by id.

In the editor: erase a browsed level's last monster that the party had killed, then place a new one. On entry the new monster gets the old hp-0 diff and arrives dead. An item can arrive already collected the same way.

- src/Game/DungeonEntities.cpp:124
- src/Game/DungeonWorld_Remote.cpp:381
- src/Game/DungeonWorld_Save.cpp:459

Fix: keep a monotonic `m_nextId`, seeded from the parse's record count. Optionally also drop the erased id's diff.

### C330. Item catalog fields are parsed twice, and the UI copy keeps stale wear slots across a world switch
*medium - bug - effort M*

Game::LoadItemIcons re-reads items, weapons and armor into the UI banks (category, weight, wear, capacity, holdable). DungeonWorld::ItemKindFor parses the same fields into ItemKind, and the two copies already differ.

The live bug: the reload clears every bank map except `wearByType`, and world teardown clears neither bank. After switching to a world where an item id no longer has `wear`, the doll still accepts it in that slot.

- src/Game/Game.cpp:926, :960, :611
- src/Game/DungeonWorld_Load.cpp:1237

Fix: clear `wearByType` with the others now (one line). Later, build the banks from the world's ItemKinds. A verifier corrected one claim: a type-editor save rebuilds neither copy, so refreshing only the UI banks would make them diverge.

Related: `ui/glow_radial` is loaded twice (src/Game/Game.cpp:921, src/Game/GameUI.cpp:227), which costs an extra SRV slot.

### C331. Dungeon `levels` and quest `stages` are split with ParseTags, which lowercases ids
*medium - bug - effort S*

The code's own comments say lowercasing is wrong for level stems and stage ids (src/Game/Project.cpp:307, src/Game/Game_Wiring.cpp:489). ParseTags still splits them in the checker, EnterLocation, the world settings dialog and ArrivalsOn.

This is latent, because all shipped ids are lowercase. But RenameLevel accepts capitals. With a stem like `Keep1`:
- the checker reports false errors;
- EnterLocation misses the stash, so that level's saved state is lost;
- a reroll stops protecting the doorway's landing square.

- src/Game/Game_World.cpp:145, :162, :278
- src/Game/Game_Generate.cpp:613

Fix: one case-keeping id splitter in Catalog.h (SplitWords is private to Project.cpp), used for every id list. Do not use Project::DungeonLevels in the checker, because it drops unknown stems and would hide the error. Alternatively, force lowercase at every id source.

### C332. Regenerating a level drops its stair `flag=` gates, atmosphere and UI material
*medium - bug - effort S*

The reroll promises stairs "carried across VERBATIM" and "a reroll changes the shape, not the look". But BuildLevelText writes stair lines without `flag=` and writes no `atmosphere` or `uistone` record, while SerializeMapStatic writes all three. The new map then replaces the old one whole.

This hits real content: crypt1, crypt2 and eval_arena all carry `atmosphere`. The lost stair gate is fully silent, because `flagunused` stays quiet while a lever still sets the flag. Undo restores everything, and nothing reaches disk before `savemap`.

- src/Game/Game_Generate.cpp:126
- src/Game/DungeonWorld_LevelIO.cpp:174, :252

Fix: share one stair-line formatter (also used by the exits at Game_Generate.cpp:206 and :403). Capture the viewed map's atmosphere and uistone before BuildAndInstall, since `viewed` aliases m_map, and carry them across.

### C336. SyncProjectToSource never copies an imported surface's worn meshes
*medium - bug - effort S*

RecordImport stores a texture import as `<set>_2k`, so the sync looks for `worn_<set>_2k*`. The baker writes `worn_<set>_<low|med|high>.gltf`, so nothing matches; ReplayImports.ps1 strips the suffix for exactly this reason. The match is also a bare `starts_with`, so a model import named `pot` copies `pottery*` and `potion*` files too.

Only a packaged build runs this. There, an editor-imported surface reaches the source tree without its worn meshes, and a fresh clone aborts in LoadModelOrDie.

- src/Game/Game_Editor.cpp:116, :735
- tools/AssetBaker/ModelBaker.cpp:2471
- tools/ReplayImports.ps1:132

Fix: strip the resolution suffix for the worn glob and match on `.`/`_` boundaries. Better, store the base name and kind in imports.cat.

### C343. Setting one entry coordinate of a world location makes the save that wrote it abort
*medium - bug - effort S*

`worldloc set <id> entryx 5` on a location with no entry leaves entryZ at -1. Serialize writes `entryx=5 entryz=-1`, and WorldMap::Load's pair assert aborts, in release too. SaveWorld re-reads the file as a check, so the save itself kills the process with the bad file already in the git tree, and every later load aborts. The dialog sets both values through a checkbox, but its number fields accept negatives: a negative Z aborts the same way.

- src/Game/Game_DevWorld.cpp:957
- src/Game/WorldMap.cpp:252, :411
- src/Game/Game_World.cpp:113

Fix: add one validated mutator that takes both coordinates or none, and route the console and the dialog through it. Refuse negatives, and have Serialize write the entry only when both are >= 0.

Lower priority: WorldMap.h:252 claims Load rejects duplicate location ids. It does not, though only a hand-written file can contain them.

### C346. The surface type editor writes a new `texture` before its worn-mesh bake succeeds
*medium - bug - effort M*

The type editor's Save writes the fields first, then starts the bake. If the bake fails, a bake is already running, or the baker cannot launch, the new `texture` stays and there is no rollback. `typeset walls <id> texture <set>` changes it with no bake at all. The next level load, palette add or quality switch then aborts in LoadModelOrDie on `worn_<set>_<tier>.gltf`.

- src/Game/Game_Wiring.cpp:556 (write), :588 (bake)
- src/Game/Game.cpp:1970
- src/Game/Game_DevWorld.cpp:1186

The finding first blamed the palette writers that skip the asset check; a verifier ruled them out, since every shipped surface type is baked at all three tiers.

Fix: write the new fields only after the bake succeeds, report failure in the dialog, and make `typeset` bake when the schema says so. Also fix the false comment at src/Game/DungeonWorld_Editing.cpp:334.

### C347. Two definitions of "is a rune": the item kind uses category plus symbol, everything else the `rune_` id prefix
*medium - bug - effort S*

ItemKindFor marks a rune by `category = rune` plus its `symbol`, and adds Memorize. But Memorize, the tablet icon and the icon loader all go through RuneSymbolFromItemId, which requires the `rune_` prefix. CreateDefaultParty also hands out `rune_<symbol>` ids directly.

Renaming `rune_fire` in the type editor re-types every placed tablet to an id that cannot be memorized and draws no glyph. Meanwhile the default party still receives `rune_fire`, now a placeholder item with no catalog entry. A new rune under any other id is just as dead, and nothing warns.

- src/Game/DungeonWorld_Load.cpp:1434
- src/Game/Spells.cpp:94
- src/Game/GameUI_Items.cpp:588
- src/Game/Character.cpp:153

Fix: make ItemKind's rune flag and symbol the one source, and look Memorize and the icon up through the kind.

### C352. The armed brush is a row index, re-resolved by rebuilding the palette at every use
*medium - bug - effort M*

The editor stores the armed brush as a category plus a row number. Surface rows come from the viewed level's palette, and browsing never resets the brush. So a brush armed as `marble` at row 2 on level A paints row 2 of level B's palette. Undoing a palette add or deleting an earlier catalog entry shifts it the same way, and the highlight moves with it without saying so.

Cost: PaintCell rebuilds the whole category list for every square, so a Fill level with the Catalogue view on makes hundreds of thousands of allocations (editor only).

- src/Game/MapEditor.h:507
- src/Game/MapEditor.cpp:991
- src/Game/MapView.cpp:135

Fix: store the brush by id, find the highlighted row by id when drawing, and look the id up once per gesture.

Against a documented decision: CLAUDE.md's guard checks the armed row number against the palette's current size. That stops an out-of-range row, not a wrong one.

### C353. Middle-click erase on an empty square adds an empty undo step and wipes redo
*medium - bug - effort S*

The erase ladder always ends with `CommitUndoStep(true)`, on the assumption that "the last rung resets". On a plain square the reset rung's `EditVariant` returns early, because the revision did not move. The commit still pushes a snapshot, clears the redo stack and calls `NoteEdit()`, which re-runs the live checker and census. The log also says the cell was reset.

So if you undo a few steps and then middle-click a plain square (or miss a target), the redo history is gone. `EraseRemote` has the same unconditional reset.

- src/Game/MapEditor.cpp:1315
- src/Game/DungeonWorld_Editing.cpp:123
- src/Game/DungeonWorld_Undo.cpp:183
- src/Game/DungeonWorld_Remote.cpp:390

Fix: commit only when something changed. Compare revisions on the live level, and make `EraseRemote` return whether it acted. Log "nothing to erase" otherwise.

### C355. Deleting a monster or prop in the editor makes the damage ledger report a false violation
*medium - bug - effort S*

`Ledger::Sweep` matches entries by address only. The editor erases from the middle of `m_monsters`, `m_decorations` and `m_doors` while the world keeps running, and never rebases the ledger. The next element slides onto the freed address, and the "outside the world update" checkpoint then compares its hp with the deleted object's baseline. The result is a violation naming the deleted monster, or a DN_ASSERT abort under `pipelineguard strict`. A guard that fires on ordinary editing teaches people to ignore it.

- src/Game/DamageLedger.cpp:66 (the header comment at DamageLedger.h:116 wrongly says reordering is handled)
- src/Game/DungeonWorld_Editing.cpp:51, :593, :645
- src/Game/DungeonWorld.cpp:1781

Fix: call `RebaseDamageLedger` from RemoveEntityAt, RemoveMonsterByRuntimeId, RemoveDecorationByIndex, RemoveDoorAt and PruneEntitiesForCell, and fix the header comment. The verifier noted that comparing keys in Sweep would not be enough. It would only help monsters, because decorations, doors and fixtures are keyed by container index.

### C365. Pausing or opening the sheet from the world map shows the parked dungeon behind it
*medium - bug - effort S*

Esc on the world map sets `Paused` with `m_resumeState = WorldMap`. Render chooses the 3D pass from `m_state` alone, so it draws the parked level, which is always resident under the world map, shadows included. The world map itself is drawn only under `case AppState::WorldMap`. The pause overlay is just a 55% wash, so the old dungeon shows through the menu. The sheet opened from the world map has the same problem, and both waste a full 3D frame.

- src/Game/Game.cpp:2211, :2761, :2864
- src/Game/GameUI.cpp:2439

Fix: add one `BackdropState()` helper (it returns the resume state while Paused or CharacterSheet). Use it for both the 3D gate and the 2D switch, so the world map draws under the overlay and the 3D block is skipped.

### C307. Read-only queries still stash levels, so the next savemap rewrites files nobody edited
*medium - rule - effort M*

These break CLAUDE.md's "NEVER STASH TO READ":
- **StairsInto** stashes every level outside the dungeon being deleted. It runs as `canDelete` when you press Delete on a dungeon, before the confirmation, and from `dungeons what <id>`. So even a refused or cancelled delete stashes the whole project.
- **RenameLevel** stashes every other level just to test its stairs.
- **EraseRemote and PruneStashRecordsForCell** create an .ent stash up front. Every remote wall paint or erase then rewrites an unchanged .ent. This is the common case.
- **Rarer cases:**
  - MapOf via PaletteDonor, only when a hidden `palette` knob is set from the console or a preset.
  - ResizeLevel, which stashes before its refusals.
  - RemovePairedStair, which stashes before checking the pair.

The cost is git churn and larger undo snapshots, and EnsureMapStash's asserting parser can abort on a broken file. Lost comments do not matter, since level files are data. The verifier narrowed one part: CellFreeForStair and FarthestStairCell stash a level that AddStairAt writes next anyway, so they only leave a stray stash when the link fails.

- src/Game/DungeonWorld_Levels.cpp:94, :131
- src/Game/DungeonWorld_Remote.cpp:226, :368, :398

Fix: add one `LevelForReading(stem)` (live, else stash, else ReadOnlyLevelOf) to replace the verbatim copies in Census.cpp:93 and Validate.cpp:197, and use it in the readers above. Stash only the levels that actually change.

### C309. The live and *Remote editing APIs restate every rule and have drifted into bugs
*medium - structure - effort L*

Most editor ops exist twice: a live form and a `*Remote` form for a browsed level (DungeonWorld.h:1880-1912). MapEditor chooses between them at about 16 `remote ?` branches. The copies have drifted:
- **Bore brush:** it has no remote arm (MapEditor.cpp:832). While browsing, it bores the ACTIVE level at the same coordinates if they qualify, and otherwise refuses for a reason the user cannot see. Undo recovers it.
- **EraseRemote** has no wall-face rung and no bore rung, and it takes items before decorations, the reverse of the live order (DungeonWorld_Remote.cpp:364).
- **AddItemRemote** counts `niche=` records toward the 4-item floor cap.
- **AddDoorRemote** lacks AddDoor's monster guard (DungeonWorld_Doors.cpp:185-225).

Fix: use the stem-taking style that AddStairAt already uses, with one `Op(stem, ...)` per op. Validation stays shared behind an occupancy query that has a live and a records version, and there is one erase ladder that takes the face. Quick wins first: refuse the bore brush while browsing, and add a bore rung to EraseRemote.

Against a documented decision: CLAUDE.md describes this remote seam. The split itself is fine. The hand-kept duplication of entity placement and erase is the problem.

### C312. The level format is parsed in DungeonMap/Entity but written in DungeonWorld
*medium - structure - effort M*

The readers are DungeonMap::Parse and ParseEntityRecord. The writers (SerializeMapStatic, SerializeRecord and KindName) live in DungeonWorld_LevelIO.cpp with their own token tables, so a new record or param needs matching edits in two modules. The writer's own comment records the cost: one savemap of crypt1 once made the demo world fatal to load. Text-only tools (populate, the generator) have to go through `DungeonWorld::EntTextOf`.

- src/Game/DungeonWorld_LevelIO.cpp:135, :150, :277
- src/Game/Entity.cpp:97
- src/Game/DungeonMap.cpp:121 (dispatches by prefix; nothing collides today)

Fix: put `DungeonMap::Serialize` beside FromText and a record formatter beside ParseEntityRecord, dispatch on `tok[0]` exactly, and add a pure RollTest round trip.

### C313. Active and stashed level writers build records separately
*medium - cleanup - effort M*

ActiveMapText writes decoration lines by hand, without a facing token for wall props. The stash path (LiveDecorationRecords plus SerializeRecord) always writes the facing. The verifier says this is latent: no shipped .map uses `wall=`, and the loader ignores facing on wall props. LoadButtons and AddButton each build a Button, where doors share one SpawnDoor.

Worth checking: ActiveEntText writes each monster's LIVE position and `asleep` flag (LevelIO.cpp:362-366), and during play only the editor removes monsters from m_monsters. So a savemap after the world has simulated could save roaming or death positions, and woken sleepers, as authored spawns.

- src/Game/DungeonWorld_LevelIO.cpp:91, :337
- src/Game/DungeonWorld_Load.cpp:1506
- src/Game/DungeonWorld_Doors.cpp:540

Fix: make ActiveMapText run SerializeRecord over LiveDecorationRecords, add SpawnButton, and add one helper for the decoration matrix.

### C314. Level text is hand-written in about five places outside the serializer, and a reroll drops stair flags
*medium - cleanup - effort M*

BuildLevelText says it carries stairs "VERBATIM", but its own stairs line omits `flag=`. So rerolling a floor silently drops any stair's flag gate. Other copies:
- **RecordLine** clones SerializeRecord. Its "no shared DirName" comment is stale, since DirToken exists (Entity.h:41).
- **Stairs lines** at Game_Generate.cpp:127, :206, :403 and Game_NewWorld.cpp:164.
- **Palette blocks** with their own join lambdas at Game_Generate.cpp:120, Game_Editor.cpp:366 and Game_NewWorld.cpp:260.
- **StyleLook** returns `theme` record text.

Fix: EntTextOf and StashedMapText already exist. Build these levels as an in-memory DungeonMap, serialize them through those, and delete the copies.

Against a documented decision: Game_Generate.cpp:575-580 justifies staging through temp files because DungeonMap could only load from a path. FromText now exists. Note that InstallLevelFromText forces m_currentLevel, so the fix is FromText plus InstallLevel.

### C315. Bool, colour and number values mean different things to different readers
*medium - cleanup - effort M*

These are all latent (shipped data uses 0/1), but they are real divergences:
- **Door `open=`** means `!= "0"` in play but `== "1"` in Validate, so `open=true` is open in the game and closed to the checker.
- **serialize::GetBool** reads `no` and `off` as true. lights.cat reads `no` as false. The type editor's checkbox shows only `1` and `true` as on. Niche `hidden=` misses the `F` that `lit=` accepts.
- **Colours:** CatalogColor takes 3-4 numbers and is lenient, while light::ParseColor takes exactly 3 and is strict. BuildLiquid re-counts tokens because CatalogColor returns no count. The verifier says the map, save, ini, stones and hex colour parsers read different formats, so leave them alone.

- src/Game/Serialize.cpp:44
- src/Game/Validate.cpp:190
- src/Game/Catalog.cpp:19

Fix: put one pure parse set in Serialize.h: a strict ParseBool that warns on junk, ParseColor returning its count, ParseInt, and one Trim.

### C317. A syntax error in any level file aborts the process, even from background scans
*medium - structure - effort M*

DungeonMap, Entity and WorldMap parsing DN_ASSERT on bad data in every build. ReadOnlyLevelOf builds these parsers for the live checker, census, type counts and browsing. So one bad level that is not loaded (a hand edit, or a keyword from another branch) kills the editor instead of showing up as a Check issue. Whitespace-only lines also abort, because ReadLevelLines keeps them. SaveWorld's read-back "warning" re-reads through the asserting WorldMap::Load.

- src/Game/DungeonMap.cpp:22, :398
- src/Game/Entity.cpp:134
- src/Game/DungeonWorld_Validate.cpp:155
- src/Game/Game_World.cpp:119

Fix: add TryParse returning {map, errors}, keeping the asserting wrapper for real loads. Report the errors as Issues in the scans and read-backs.

Against a documented decision: Assert.h says "silent corruption is worse than a crash". A returned error is just as loud without being fatal.

### C318. The .map parser copy-pastes its number-or-assert helpers
*medium - cleanup - effort S*

The strict `from_chars` plus DN_ASSERT lambda is redefined about eight times in DungeonMap.cpp and again at Entity.cpp:107. WorldMap.cpp:26 already has ParseCoord, ParseNumber and SplitParam for the same dialect. The typed-first-token probe is tripled, and the variant and theme records repeat the same surface dispatch.

- src/Game/DungeonMap.cpp:143, :233, :572, :603

Fix: move RecordInt, RecordFloat (taking a `what` label to keep the error wording) and SplitKeyValue beside SplitRecordTokens in Entity.h, and fold variant and theme into one helper.

### C319. Catalog categories are registered in several hand-kept lists, and surface features dropped out of the rename sweep
*medium - structure - effort M*

kCatalogs (Project.cpp:27) already pairs each file with its member. Even so, CatalogForKey, AllCatalogs, SchemaFor, DefaultMount and SweepTypeRefs each hand-list the keys. MapEditor's kCategoryInfo holds category identity that the type editor needs.

One real bug: SweepTypeRefs has no `surfacefeatures` branch (DungeonWorld_Editing.cpp:255). Renaming a surface feature type therefore leaves feature records naming the old id, and Delete's in-use check lets a used type be deleted. The verifier noted that leaving races, genpresets and imports out of CatalogForKey is documented and intended.

Fix: make kCatalogs the one registry, derive the other lists from it, and check at startup that every kCategoryInfo key resolves.

### C320. Member effects have their own save conversion and save line, which drop tint and source
*medium - cleanup - effort S*

Effect lists are saved through three hand-written copies:

- Monsters and breakables use CaptureEffectList/RestoreEffectList (src/Game/DungeonWorld_Save.cpp:20-55).
- Game re-implements both for party members without the tint branch (src/Game/Game.cpp:1205-1208, 1341-1349).
- The member `effect` line (src/Game/SaveGame.cpp:220) writes no source and no tint. The `enteffect`/`brkeffect` lines (:331, :376) write both. The enteffect reader takes `tok[1]` raw, while brkeffect uses DeTok (:661 vs :770).

The verifiers found that nothing is lost today: no current path tints a member's effect or gives it a source. The risk is that the next EffectState field gets missed in one of the copies.

Fix: let Game call the shared helpers, and use one write/parse pair for all three lines. The member line can gain optional trailing source/tint tokens without a version bump.

### C321. The save reader keeps migration code for versions it refuses, and the docs cite the retired version ladder
*medium - cleanup - effort S*

The save format restarted at v2 (src/Game/SaveGame.h:36-54, `kMinReadableVersion = 2` at :326), and Refused() rejects anything older. ReadSave still handles shapes that no accepted save can contain:

- the v1 `level` key (src/Game/SaveGame.cpp:88) and the v1 top-level fold (:424-435)
- the v13 `shield` line (:538-555)
- the flat pre-v16 usedef/mru lines (:503-520, :615-626)
- LoadGame's base back-solve (src/Game/Game.cpp:1383-1400)

CLAUDE.md still cites v14/15/16/18/22/25, Refused() says "THE FLOOR (v26)", and all three effects.cat headers still point authors at the old poison/bleed/element_dot aliases.

Fix: delete those branches and FindLegacy. Keep the hasAttrs/hasBases/hasSupplies defaults, because the files are meant to be hand-editable, and keep the `tok.size()` guards, because they also bound indexing. Rewrite the vNN references to name lines only.

### C322. settings.ini scalars are listed twice with an unanchored key search, and slider ranges are stated twice
*medium - structure - effort M*

- About 45 scalars are hand-listed in Load (src/Game/GameSettings.cpp:163-273) and again in Save (:303-354).
- Every ParseIni* helper finds its key with an unanchored `text.find(key)` (:93, :107, :121, :135). This is latent: no key collides today.
- `language=`, `quality=`, `maxlights=` and the `key_` loop hand-roll the helpers with magic offsets.
- About nine slider ranges are repeated between the Load clamp and the page (src/Game/GameUI.cpp:514-519). If you widen a slider and not the clamp, the next launch clamps the value back.
- BalanceField (src/Game/Balance.h:439) has no kind, range or help, so BalanceDialog.cpp:114-135 matches the curve keys by name.

Fix:
- Move the scalars into a table like kThemeFields and parse it line by line.
- Give BalanceField kind/range/help.
- Delete CLAUDE.md's stale `map_player_key_collapsed` line.

The verifier advises against merging all six descriptor tables into one, because they do different jobs.

### C324. The breakable profile is parsed three times and seeded three ways, with different defaults
*medium - cleanup - effort M*

- **Parsed three times.** hp/armor/resists are read for decorations (src/Game/DungeonWorld_Load.cpp:1953-1959), fixtures (:2010-2016) and doors (src/Game/DungeonWorld_Doors.cpp:81-88). Doors default hp to 40, the others to 10. Doors are also parsed through DecorationKindFor first, so a bad `resists` warns twice, the first time under "decorations.cat".
- **Seeded three ways.** SeedBreakable (src/Game/DungeonWorld_Combat.cpp:2599-2606) calls itself the one place, but it takes only decorations. Fixtures and doors seed inline.
- **Enumerated again.** ForEachBreakableAt also claims to be the one place, but TickBreakables and the save capture/restore loops walk all three lists themselves.

Fix:
- Read a BreakableSpec once per kind, with the hp default passed per catalog, and seed it through one Seed function.
- Route ticks and saves through a ForEachBreakable with per-kind hooks (a broken door opens for good, a fixture goes dark).

### C335. InstallLevelText round-trips through fixed %TEMP% files
*medium - cleanup - effort S*

InstallLevelText (src/Game/Game_Generate.cpp:573-597) writes the level to `%TEMP%/dungeon-gen/<stem>.map/.ent` and installs it through InstallLevelFromFiles, which has no other caller. Its reason, that DungeonMap "only constructs from a path", is stale. FromText exists and InstallLevelFromText uses it (src/Game/DungeonWorld_Validate.cpp:79-80). PopulateViewedLevel parses, re-serializes, writes the temp files and parses again (src/Game/Game_Populate.cpp:81, 146).

Consequences:
- Parse errors name a temp path instead of the level.
- Because the path is fixed, two game processes rerolling the same stem could install each other's level. The window is only milliseconds.

Fix: add a text install that parses with FromText and calls InstallLevel, without the encounter-only `m_currentLevel = stem`. Delete InstallLevelFromFiles.

### C338. The "always someone" rule and other generator blocks are copied
*medium - cleanup - effort S*

The fallback "no monster placed, so put one at the exit" is pasted twice (src/Game/Game_Generate.cpp:189-198, 372-381) and takes `p.monsterIds.front()`. Populate picks a rank-matched monster instead (src/Game/Generate.cpp:1065-1083). The wizard copy's comment says "the weakest of the pool", but FillPools never sorts by threat. For a style list like "giant_spider 2, centipede 2, blob 1", the spider is the one that appears.

Smaller copies:
- the floor-above lookup (Game_Generate.cpp:533, 633)
- the shared BuildBlankWorld/BuildWizardWorld preamble (Game_NewWorld.cpp:234-245, 278-289)
- the "in memory, else on disk" readFile lambda
- the catalogKey-to-surface mapping, three times in Game_Editor.cpp

Fix: make the guarantee a Run option, off by default so unstyled output does not change. Add small shared helpers for the rest.

### C339. Run and Populate duplicate their density formulas, rank sort and place lambda
*medium - cleanup - effort M*

generate::Run and Populate share copied code (src/Game/Generate.cpp:771-827 vs 1009-1038): the knob clamps, countOf, the threat-ranked stable_sort, the `place` lambda, and the 0.04/0.03 per-square density formulas. Tuning one constant changes only one path, although Generate.h:162-163 promises "the same knobs mean the same thing". PickNear's comment (Generate.cpp:88-90) says equal weights reproduce Run's uniform step. They do not at the band's edges: at centre 0, Run picks rank 0 with odds 2/3 and PickNear with odds 1/2.

The verifier narrowed this. The rest of the two functions differs for real reasons. Run's unweighted pick is also kept on purpose, so unstyled output stays byte-identical (Generate.cpp:851-856).

Fix: extract a small shared density/rank/place helper that keeps Run's RNG call order, and correct PickNear's comment.

### C340. The 2x2 room rule is coded three times, and the doorway rule twice
*medium - cleanup - effort S*

The room test ("in some 2x2 walkable block") is copied verbatim in three places:

- area::IsOpen (src/Game/Area.cpp:12-21)
- carve::Dress (src/Game/Carve.cpp:228-236)
- Populate (src/Game/Generate.cpp:911-919)

Generate's IsDoorway (Generate.cpp:463-475) also restates DungeonMap::DoorwayFacing (src/Game/DungeonMap.cpp:870-878). The copies exist because Carve and Generate are pure TUs linked into RollTest without DungeonMap. If the rule changes in one place, the Area fill, the style dresser and the populator will disagree on what a room is.

Fix: put predicate-templated room and doorway helpers in a pure header and use them everywhere. One finding also said the flood fill is copied three times. That part is overstated: the three floods use different predicates and outputs.

Against a documented decision: Generate.cpp:460-462 restates DoorwayFacing because the module "deliberately owns no map". A predicate template keeps that purity.

### C294. The throw cooldown and kindle clock survive a reset, and an eval reset mid-rest can leave lockstep forced on
*low - bug - effort S*

m_throwCooldown (src/Game/DungeonWorld.h:3401) and m_kindleClock are not cleared by ResetForNewGame or ResetForEval (src/Game/DungeonWorld_Save.cpp:58-183). A new game, a load or an eval `reset` can inherit up to 1 s of throw wait. ResetForEval also sets `m_resting = false` directly (:181), skipping the lockstep hand-back. No current suite hits that case.

Fix: zero both, and end rest through a quiet SetResting(false).

### C308. Leaving a level always stashes its .map, so savemap rewrites levels that were only visited
*low - bug - effort S*

BeginLevelLoad (src/Game/DungeonWorld_LevelIO.cpp:37-43) and ParkActive stash the .map on every exit, while the .ent stash is gated by m_entsDirty. SaveAllLevels then rewrites every stash. Walking crypt1 -> crypt2 -> crypt1 and saving rewrites crypt2.map, which shows up in git status although nobody edited it. That goes against editor-updates P0's "only touched levels" rule.

Fix: add a map-dirty flag that mirrors m_entsDirty.

### C323. Some editor writes still drop catalog comments
*low - bug - effort S*

CLAUDE.md says catalog comments survive a write, but three paths lose them:

- ParseBlocks drops comments after the last block (src/Game/Serialize.cpp:144-149). New worlds therefore lose the template's header-only flags.cat, quests.cat and dungeons.cat documentation.
- Saving from the monster config dialog moves threat_threshold to the end and deletes its comment (src/Game/Game_Editor.cpp:1233-1253).
- Deleting a catalog's first entry deletes the file header (src/Game/Catalog.cpp:162-168).

Fix: keep a file trailer, set fields in place, and move a deleted first entry's comment block to the next entry.

### C348. Painting the start square to rock and then resizing writes a level with no P
*low - bug - effort S*

SetCell has no start guard (src/Game/DungeonMap.cpp:1361), and the writer emits 'P' regardless (src/Game/DungeonWorld_LevelIO.cpp:193), so a painted start silently reverts on reload. ResizeLevel counts only walkable squares outside the window (src/Game/DungeonWorld_Resize.cpp:86-96), so it can crop a rock start. The next parse then aborts with "map has no 'P' start cell" (DungeonMap.cpp:117).

Fix: refuse structural paints on the start square, and count the start in the resize check.

### C349. Hand-edited save with a bad member or pack index is not refused
*low - bug - effort S*

In a hand-edited save, a `char -1` or `packc <i> -1` line makes `resize(idx + 1)` shrink the vector to zero. The next subscript then reads out of bounds, which aborts in debug and is undefined behaviour in release. Other negative or very large indices throw length_error or bad_alloc. Main's per-frame try/catch logs that as a failed frame, so the game survives, but the load is never cleanly refused. A truncated or corrupted file will not realistically produce these values; only a deliberate edit will.

- src/Game/SaveGame.cpp:66-70 (CharAt)
- src/Game/SaveGame.cpp:627-633 (packc)

Fix: skip or refuse any line whose member index is outside [0, party::kMaxMembers) or whose pack index is outside [0, kPackRowSlots), and parse the numbers with std::from_chars.

### C356. Door inspector can shut a smashed door or close it on a monster
*low - bug - effort S*

The inspector's apply calls SetDoorSettings, which writes `door->open` directly. It skips both checks ToggleDoor makes: the broken-door check and the "something is in the doorway" check. In the live editor you can therefore shut a wrecked leaf (ToggleDoor then refuses to open it) or close a door on a monster. The effect is editor-only and lasts one session, because loading a save forces a broken door open again (src/Game/DungeonWorld_Save.cpp:547). ToggleDoor's comment claims the inspector goes through it, but it does not.

- src/Game/DungeonWorld_Doors.cpp:385-392
- src/Game/Game_Wiring.cpp:976
- src/Game/DungeonWorld_Doors.cpp:228-230 (the wrong comment)

Fix: let the inspector change the authored initialOpen, but keep the live `open` true when the door is broken and refuse a close while the doorway is occupied. Then correct the comment.

### C357. Closing door checks for monsters but not the party
*low - bug - effort S*

ToggleDoor only refuses to close when a monster is in the doorway. AddDoor and MoveObject both test for the party as well. In play, this is only reachable through a lever placed in the doorway itself and wired to that same door. AddButton accepts that placement and Validate does not flag it. Pulling such a lever shuts the door on the party, and the closed square then works as a free shelter that bolts and blasts cannot enter.

- src/Game/DungeonWorld_Doors.cpp:235-239 (ToggleDoor)
- src/Game/DungeonWorld_Doors.cpp:188-191 (AddDoor)
- src/Game/DungeonWorld_Move.cpp:256-260 (MoveObject)

Fix: write one `DoorwayOccupied(x, z)` helper that checks the party or any monster, and use it in all three. Optionally, add a Validate rule against a lever wired to the door it stands in.

### C359. Floor-item click test uses the wrong height
*low - bug - effort S*

PickItemIndex tests the click ray at half the model's bind-pose height, measured in model units. The floor draw scales by kUnit and lays rods and slabs flat. A torch is therefore tested at 0.12 m, but it is drawn at about 0.045 m. The error is about 0.1 m against a 1.25 m quarter square, so it is small. GroundOffsetY, which was meant to be the shared answer, has no callers.

- src/Game/DungeonWorld_Load.cpp:1593
- src/Game/DungeonWorld_Render.cpp:580 (FloorItemWorld)
- src/Game/DungeonWorld.h:2530 (unused GroundOffsetY)

Fix: take the height from FloorItemWorld's grounded bounding box and cache it per kind. Delete GroundOffsetY and the stale comment at src/Game/DungeonWorld_Render.cpp:574. Recheck the fallback constants for tablets with no model (0.23 / 0.45) at the same time.

### C360. `materials[0]` read without a check for models with no material
*low - bug - effort S*

LoadGltf creates one MaterialData per glTF material and nothing more (src/Assets/Model.cpp:267-289). A valid glTF with no material therefore gives an empty list. About a dozen sites index `materials[0]` without checking: src/Game/DungeonWorld_Load.cpp:867/1979/2022/2045, src/Game/DungeonWorld_Render.cpp:597/640/671/1315, src/Game/AssetDialog.cpp:423 and src/Game/Game_DevCommands.cpp:1036. Such a model aborts in debug and is undefined behaviour in release, and for a monster that happens every frame. The rune model is guarded in FillItemPreview but not at Render.cpp:597/640. Only an outside glTF can reach this, because AssetBaker always writes a material.

Fix: have assets::LoadModel always supply at least one default material.

### C361. `resource::Rules{}` is not the "inert default" the code claims
*low - bug - effort S*

The practice curves in Rules default to CurveRules{}, which means slope 5 and cap 120. SkillTerm only switches a term off when the cap is 0 or below. So `RecomputeMaxima({})` still adds a practice term, even though its comment calls the rules "inert defaults". A created member who picks conditioning starts with about 9 extra maximum stamina. The result is correct today only because the preview and ResetRoster recompute with the real rules straight afterwards.

- src/Game/Resource.h:82, 93
- src/Game/Character.cpp:121-124
- src/Game/Game_Party.cpp:189-190

Fix: give these curve members a default cap of 0 (also in SupplyRules) so that `{}` really is inert.

### C362. Curve-form knobs are cast to the enum with no range check
*low - bug - effort S*

Balance::Load copies the float from balance.cat unchecked, and six sites cast it straight to CurveForm. Only the Balance dialog clamps the value. Any out-of-range value falls to the `default:` branch in src/Game/Curve.cpp:25, so every skill, stat, avoid, resource and pace curve silently becomes logarithmic. A negative value is also shown wrongly: the dialog clamps its selection to 0 and displays "hyperbolic" (src/Game/BalanceDialog.cpp:136).

- src/Game/Balance.h:390/403/408/412
- src/Game/Balance.cpp:237, 275, 311-312

Fix: clamp both form knobs in Balance::Load, and replace the casts with validated `SkillForm()` and `StatForm()` helpers.

### C364. New game that needs a level load starts with an empty log
*low - bug - effort S*

StartNewGame returns as soon as OpenInLevel stages a load. That skips the opening message, the key help and ResetHudStatus. When the load completes, the handler only clears the log. So Start New Game from a session left on crypt2 (or eval_arena) starts in silence, while the same action from crypt1 shows the intro.

- src/Game/Game.cpp:1097, 1106, 1126 (early returns)
- src/Game/Game.cpp:1130-1135 (the skipped intro)
- src/Game/Game.cpp:2125 (load completion)

Fix: move the intro into a shared `BeginPlay()` tail, or set an `m_pendingNewGame` flag that the LoadingLevel completion acts on.

### C367. Failed save-slot delete gives no message
*low - bug - effort S*

The save row's delete calls `std::filesystem::remove(path, ec)` and never reads `ec`. If the file is locked or read-only, it simply reappears in the list, with no message and no log line.

- src/Game/GameUI.cpp:980-985

Fix: add `bool DeleteSave(path, std::string& why)` on the SaveGame side that logs the failure, and have GameUI show it. The verifier found the finding's layering argument overstated: GameUI already calls SaveGame's free functions directly. The duplicated WorldTitle lookup (src/Game/GameUI.cpp:29) is cosmetic.

### C316. Catalog list and `resists` parsing is duplicated, and `cures` splits differently from `on_hit`
*low - cleanup - effort M*

Two of these copies change behaviour:

- **`cures` vs `on_hit`:** `on_hit` (src/Game/Effect/Effect.cpp:110) accepts `,` or `;` between groups, but `cures` (src/Game/DungeonWorld_Load.cpp:1271) accepts only `,`. So `cures = poison 0.5; bleed` silently drops bleed.
- **Resist re-parsers:** RaceTraits (src/Game/Game_Party.cpp:65-72) and threat::PhysicalResist (src/Game/Threat.cpp:30-39) parse `resists` with istringstream and skip DamageTypeBook. They ignore aliases and unknown types, and stop silently at a bad token. The party page can therefore show different resists from the ones ApplyRaceResists applies.

The rest is plain duplication: the space/comma splitter (Balance.cpp:153, DungeonWorld_Load.cpp:50, Game.cpp:932, Project.cpp:89, Game_World.cpp:47, Game_Wiring.cpp:491) and three copies of the "x,z" cell parse.

Fix: put shared splitters (SplitIds, SplitGroups, ParseCell) in one pure header. It must stay pure because RollTest links these files. Have RaceTraits read the ResistTable that ParseResists builds.

The verifiers narrowed the claim:

- The fixture colour assert matches the map loader's policy.
- PartyRules' SplitList is rightly comma-only.
- Tags (lowercased) and level stems (whitespace-only) differ for real reasons.

### C325. No `CatalogFloat` helper; stair fields re-read at about 15 sites
*low - cleanup - effort S*

Catalog.h has CatalogGet and CatalogBool but no float version, so the null-check pattern `def ? def->GetFloat(k, d) : d` is written out 21 times. Some defaults are repeated, for example `open_seconds` 0.7 in src/Game/DungeonWorld_Doors.cpp:434 and src/Game/Game_Inspect.cpp:157. Stairs have no resolved kind struct, so exit, traverse, up, fall, hole and pair are re-read from stairs.cat at about 15 sites, each with its own default.

- src/Game/Catalog.h:58-66
- src/Game/DungeonWorld.cpp:312-328

Fix: add `CatalogFloat`, and cache a `StairKind` per type. Keep FloorHoleAt's and CeilingHoleAt's deliberately different `hole` defaults.

### C328. Surface heights and factors stored twice; per-surface code repeated three ways
*low - cleanup - effort M*

ResolveSurfacePalettes fills `m_*Heights` and `m_*Factors`, then copies them into `Surface::heightScale` and `Surface::factors` at several sites.

- src/Game/DungeonWorld.h:4115-4119
- src/Game/DungeonWorld_Load.cpp:105-118, 177, 2336

The verifiers corrected two points:

- `loadedSets` is not a duplicate. It records what is currently loaded, and Surface::Holds (src/Game/DungeonWorld_Load.cpp:176) uses it to skip a reload, so keep it.
- The three material-override sites differ for stated reasons, so merging them saves little.

The rest is repetition with no behaviour difference:

- Callers rebuild `DungeonMap::Palette(Surface)` with ternaries (src/Game/DungeonWorld_Editing.cpp:132/346, src/Game/MapEditor.cpp:1224), even though SurfaceSel is an alias of game::Surface.
- DungeonMap's setters exist only per surface (src/Game/DungeonMap.h:312).
- MapEditor writes out SelFor's mapping again in src/Game/MapEditor.cpp:983, 1085 and 1216, and there is a matching switch in src/Game/MapView.cpp:1014.
- ResolvedVariant takes an `int` instead of the enum.

Fix:

- Keep heights and factors only in Surface, held as an array indexed by game::Surface.
- Add Surface-indexed `SetVariant` and `AddToPalette` on DungeonMap.
- Use `Palette(sel)` and `SelFor` everywhere, and type ResolvedVariant's parameter as SurfaceSel.

### C329. CatalogSchema repeats the three surface tables and the `light` row
*low - cleanup - effort S*

The wall, floor and ceiling tables (kWallFields, kFloorFields, kCeilingFields) have the same 9 rows. They differ only in the height_scale and relief defaults and the relief help text (src/Game/CatalogSchema.cpp:82, :106, :129). The weapons and armor `light` rows are identical (:583, :617).

- Fold the three tables into one SURFACE_ROWS(hsDef, reliefDef, reliefHelp) macro, and the two `light` rows into one LIGHT_ROW.
- Fix the stale comment "Same as walls minus the pillars" at :103, since `columns` has been retired.
- Leave the breakable rows alone: their help text is different in each category on purpose.

### C337. Which catalogs count as items differs per site
*low - cleanup - effort S*

The door inspector looks for keys in items.cat only (src/Game/Game_Inspect.cpp:136). FillPools and the checker search all three item catalogs through AllItems() (src/Game/Game_Generate.cpp:258, src/Game/DungeonWorld_Validate.cpp:178). This difference does nothing yet, because every key is in items.cat.

The loot difference does affect play now. The loot pool is built from items.cat alone (Game_Generate.cpp:253), so generated and populated levels never drop weapons or armor, and nothing records that as a decision.

Fix: add Project::IsKeyItem() and use it at all three key sites. Decide which catalogs feed loot in one place.

### C341. Style and theme fields are read several different ways
*low - cleanup - effort S*

StyleLook.h promises that every caller reads a style the same way, but:

- MapEditor_Shapes has its own KnobsOf, its own StyleEntry, and its own direct reads of the room, corridor and corridor_width fields (src/Game/MapEditor_Shapes.cpp:24, :43, :103).
- The `knobs` decode is repeated at Game_Generate.cpp:328 and Game_Populate.cpp:32.
- Theme members are read in three ways: by ThemeMembersOf (src/Game/DungeonWorld.cpp:52), by a FirstWord copy in StyleLibrary.cpp:22, and directly in StyleLook::Lay (src/Game/StyleLook.cpp:58).

A theme written as an old-style list would paint with the brush but be dropped by Lay. No catalog or tool writes one, so this cannot happen today.

Fix: move ThemeMembersOf and the wall/floor/ceiling name table into a pure header. Add stylelook::Knobs and stylelook::ThemeFor, and use them in all of the places above.

### C342. SplitKnobs re-splits Encode's output to drop the seed
*low - cleanup - effort S*

SaveGenPreset encodes the settings as a string, splits the string again, and drops any pair that starts with "seed:" (src/Game/Game_Generate.cpp:447). SplitKnobs (:492) has no other caller. The knob table already marks the seed as KnobKind::Seed (src/Game/GenerateKnobs.cpp:53).

Fix: add an Encode overload that skips Seed knobs and delete SplitKnobs. Then renaming the seed key cannot let seeds leak into presets.

### C350. Each undo step copies every stashed level
*low - cleanup - effort M*

CaptureEditorState makes a full copy of every stashed map and .ent (src/Game/DungeonWorld_Undo.cpp:41-44). BeginUndoStep takes that copy before it knows whether anything will change.

The verifier found the cost is small:

- The undo and redo stacks together hold at most about 64 snapshots (not 64 + 64).
- History is cleared on a level change and on a rename.
- Levels are a few KB each (crypt1 is 14x10).

Fix: hold off on copy-on-write stashes until levels get much bigger. For now, note the size assumption in the header comment (src/Game/DungeonWorld.h:4622).

Against a documented decision: CLAUDE.md and that header describe copying the whole editor state per undo step as a deliberate trade for simplicity.

### C354. Fixture `mount` is read again at seven sites
*low - cleanup - effort S*

MountFor (src/Game/Placement.cpp:30) is meant to be the one rule for deciding whether something mounts on a wall or the floor. The world's add paths read the field again themselves, as `Get("mount","floor") == "wall"` (src/Game/DungeonWorld.cpp:45, DungeonWorld_Editing.cpp:484, DungeonWorld_Remote.cpp:343, Game_Wiring.cpp:702).

They agree with MountFor today. The only mismatch is a fixture authored `mount = doorway`: the editor's preview would show it in a doorway, but it would be placed as a floor fixture.

Fix: read the cached FixtureKind::wallMount (DungeonWorld_Load.cpp:2004) or call MountFor. doors.cat also uses the name `mount` for something else. That clash is already guarded (Placement.cpp:40), so renaming the field is optional.

### C358. NicheItemPos hard-codes a catalog id and the niche's shelf height
*low - cleanup - effort S*

An item in a niche sits at 0.30, or at 0.20 when the type id is exactly "niche_arch" (src/Game/DungeonWorld_Doors.cpp:619, test at :627). Those numbers are kept in step with ModelBaker by hand.

If an arch niche type is duplicated or renamed, or a new niche model is added, items float above the shelf.

Fix: add a wallfeatures.cat `shelf` field (default 0.30) and drop the id test.

### C363. PartyRules' stat count is not tied to kStats
*low - cleanup - effort S*

PartyRules' party::kStats = 5 (src/Game/PartyRules.h:27) and Character's kStatCount = 5 (src/Game/Character.h:69) are set separately. Yet Game_Party.cpp:131 and PartyCreationPage.cpp:371, 469 and 570 index Character's kStats table by party::kStats.

If a stat is added, its race modifiers are silently never applied. If one is removed, the code reads past the end of the table.

Fix: add a static_assert in Game_Party.cpp, which sees both. PartyRules.h cannot include Character.h because it has to stay pure for RollTest.

### C368. SoundBank's sound count is repeated as a literal
*low - cleanup - effort S*

SoundBank::All() returns a 9-element array (src/Game/SoundBank.h:33). Game's voice reservation sizes its `formats` array to 9 by hand (src/Game/Game.cpp:637).

Fix: size it from `m_sounds.All().size()`.

The risk is small. The compiler rejects an All() list longer than its declared size. Voices are reserved per sound format, so a sound missing from All() only goes without a voice if no other sound shares its format.

### C369. WorldState::seen is searched linearly for every cell drawn
*low - cleanup - effort S*

WorldState::Seen searches a list of explored cells from start to finish (src/Game/WorldMap.cpp:121). WorldMapView calls it once per cell every frame (src/Game/WorldMapView.cpp:277), so each frame costs the number of cells times the number of explored cells.

That is negligible at the demo world's 24x16. If worlds get much bigger, keep a width x height mask at runtime as DungeonWorld does, and convert to the list only on save and load.

The header comment is correct: it describes the shape the save file stores.

## 10. UI library and HUD

### C370. Changing language with the party page open leaves it writing to freed widgets
*high - bug - effort S*

`RebuildForLanguage` clears `m_savesUi` (src/Game/GameUI.cpp:1292). That context also holds the party creation page, but the function only rebuilds the Saves and Worlds pages afterwards (GameUI.cpp:1307-1308). `m_menuPage` stays `Party` and nothing marks the page for a rebuild. On the next frame, `UpdateMenu` calls `m_partyPage->Tick()` (GameUI.cpp:2118-2125), which writes text and enabled flags into widgets that have already been destroyed (src/Game/PartyCreationPage.cpp:450-477). In debug this most likely crashes. In release it corrupts the heap. Before that, the page draws blank.

How it is reached: run `lang <code>` (src/Game/Game_DevCommands.cpp:93), or an eval script, while the party page is open. The bad write happens on the first frame after the console closes. A player cannot reach it from the UI, because the language dropdown is on the Settings page.

Fix: in `RebuildForLanguage`, add `if (m_menuPage == MenuPage::Party && !m_partyLeavePending) BuildPartyPage();`. `Build` nulls the cached pointers and the member specs survive. Better still, use one switch that rebuilds whichever `m_savesUi` sub-page is showing, so a fourth page cannot be missed. Also add a `lang` step to partypage.eval.

### C373. Editor wheel zoom drifts away from the cursor
*high - bug - effort S*

In Editor mode, `ComputeTransform` shrinks the fit by twice the edge-handle band (src/Game/MapView.cpp:309-312). The wheel-zoom block in `Update` works out the fit again inline, without that margin (MapView.cpp:603-614). As a result, the point under the cursor moves on every wheel step, by about 5% of its distance from the map centre. Near the edge at high zoom, the verifier measured roughly 200 px per step on a 32x32 map.

The pan is also rewritten when the zoom is already clamped at 1 or 10:
- Extra ticks at a limit keep sliding the map.
- One wheel-down at zoom 1 pushes the map off-centre, into the edge-handle margin.

Player mode is unaffected, because its margin is 0. Painting and picking are also unaffected. The margin was added later (3f7e6108), and the inline copy was never updated.

Fix: copy WorldMapView's before/after pattern (src/Game/WorldMapView.cpp:198-208). Take the transform before and after the zoom change and correct `m_pan` by the difference. A shared `ZoomAt` helper is an optional follow-up.

### C371. Item, spell and effect descriptions are cut at 255 bytes, sometimes mid-character
*medium - bug - effort S*

The details dialog reserves 600 bytes for a description (src/Game/ItemDetailsDialog.cpp:45), but the text arrives through a `loc::Line` (ItemDetailsDialog.cpp:61-74, 266-270). `Line::Assign` cuts at 255 bytes and does not check UTF-8 boundaries (src/Core/Loc.cpp:108-112). The sheet's effect and spell rows go through `FormatLine` and are cut the same way (src/Game/CharacterSheet_Lists.cpp:274, 303).

Shipped texts over the limit:
- `rune_light` in de, es, it and ru.
- In ru only: `rune_explode`, `moonstone_amulet`, `rune_multiple`, and the firelight, skylight and stonelight spell descriptions.

German loses its last sentence, which says memorizing uses up the tablet. In Russian, four of these texts end on a cut character, which draws as a '?'. Every caster starts with a Sowilo tablet, so players see the cut `rune_light` text.

Fix: when the key exists, assign `loc::View(key)` directly. Also make `Line::Assign` back off to a whole character.

Against a documented decision: Loc.h says a line longer than the buffer is a bug in that line. That rule suits log lines, but not paragraph-length descriptions.

### C372. The armor tooltip no longer shows the avoidance-skill bonus
*medium - bug - effort S*

`DefenseFor` takes the avoidance-skill bonus out of `stance` (src/Game/DungeonWorld_Combat.cpp:911-934). The armor tooltip's eight rows never show it (src/Game/CharacterSheet_Inventory.cpp:425-483). Commit 05481e7e removed the old avoidance row, and the `sheet.def.avoid` / `sheet.def.withlevel` keys are now unused in the .lang files. For an unarmored member, base + DEX + stance - armor penalty comes out short of the Roll shown by exactly the avoidance points. Anyone tuning the avoid curve from this readout will be misled.

Fix: restore the row, shown when either compared side has no armor. The rows array is already full and `rows[rowCount++]` has no bounds check, so grow it to 9 and assert in `add`.

### C379. Hidden dev-console buttons still take clicks after scrolling out of view
*medium - bug - effort S*

`DevConsole::Update` tests every left click against the previous frame's rects, with no check that the click is inside the panel (src/Game/DevConsole.cpp:249-254). The panel is only clipped when drawn (DevConsole.cpp:443), so rows laid out below it sit invisibly under the scrollback. The checkboxes (DevConsole.cpp:395) and the profile tree rows (src/Game/DevConsole_Profile.cpp:1236) guard against this. These do not:
- the thread buttons (src/Game/DevConsole_Threads.cpp:157)
- the health strips (src/Game/DevConsole_Health.cpp:228)
- the section expanders (DevConsole.cpp:376-384)

With THREADS expanded and pushed below the panel, a click in the empty scrollback can halt, re-rate, kill or reboot an AI worker, or toggle a section and rewrite settings.ini. The verifier rated it medium rather than high: it is dev-only and needs the panel to overflow, and a kill can be undone with the `boot` button.

Fix: wrap the click block and `ProfileHover` in `my < m_panelH`.

Against a documented decision: the console deliberately avoids `ui::ScrollArea` (DevConsole.h:642-650). That choice can stand, but CLAUDE.md requires that a scrolled-out row takes no input, and only drawing is clipped here.

### C375. The map toolbar is implemented twice, and the copies have drifted
*medium - cleanup - effort M*

MapView and WorldMapView each build the same toolbar by hand:
- the same right-to-left `add` lambda (src/Game/MapView.cpp:256, src/Game/WorldMapView.cpp:63)
- the same band fill, disc drawing and hover brightness
- a label trim that cuts bytes, not characters (MapView.cpp:1623, WorldMapView.cpp:386, src/Game/MapEditor_Categories.cpp:285)
- a tooltip box written six times, because `ui::DrawTooltip` is private to src/UI/Controls.cpp:193

The copies already differ:
- Only WorldMapView clears its hover before opening a modal.
- Only MapView_Docks trims with `ui::FitText`.
- The minimum button sizes are 16 vs 22 px, although a comment says they match.

The verifier narrowed two points: the byte trim only shows when an icon is missing, and the size mismatch only on panels shorter than about 524 px.

Fix: one composed `MapChrome` helper that owns the item list, layout, hit test, disc drawing and tooltip. Export `ui::DrawTooltip`, and trim with `ui::FitText`.

Against a documented decision: WorldMapView.h:9-14 chooses copying the convention over a shared base class. A helper object used by both views, rather than inheritance, keeps that choice and removes the drift.

### C374. A toolbar tooltip stays visible under the dialog its button opened
*low - bug - effort S*

A toolbar click that opens a modal leaves `m_hoverBtn` set (src/Game/MapView.cpp:695). Game then skips `MapView::Update` but still calls `Render` (src/Game/Game.cpp:2313, 2844), so a stale tooltip and lit button sit under the dialog's dim until it closes. Cosmetic only. Fix: clear the hover before firing the callback, as WorldMapView does (src/Game/WorldMapView.cpp:152). Then replace the two existing ad-hoc "did Update run" flags with one that clears every hover field.

### C380. Clicking a health mark reports a different event than its colour shows
*low - bug - effort S*

Each cell in a health strip is coloured by its most severe event (src/Game/DevConsole_Health.cpp:79-81). A click on it reports the newest event of any kind instead (DevConsole_Health.cpp:88, 117). A stall followed by its restart draws as a stall but reports the restart. Fix: store the index of the newest event of the worst kind.

### C381. HealthRow::prev is sized 6 by hand instead of by diag::kKindCount
*low - bug - effort S*

`HealthRow::prev` is declared as `u64 prev[6]` (src/Game/DevConsole.h:546), but it is indexed up to `diag::kKindCount` (src/Game/DevConsole_Health.cpp:74), which happens to be 6 today (src/Core/Diagnostics.h:60). Adding a seventh diagnostic kind would silently overwrite the row's cells every 50 ms. This is a hidden trap, not a live bug. Fix: size the array by `diag::kKindCount`, or add a `static_assert`.

### C376. The player map still tints cells from the raw variant value
*low - cleanup - effort S*

The player map tints cells with `VariantTint`, which reads the raw variant value (src/Game/MapView.cpp:1048-1050). Only cells pinned to index 1 or higher get a tint. Hash-mixed and theme-painted cells never do, so the tint reflects editing history rather than what is drawn. The verifier narrowed this: a comment just above (MapView.cpp:999) says Player mode keeps flat inks, so this is leftover code. Tinting through `ResolveSurfaceVariant` would make mixed floors blotchy. Fix: delete `VariantTint`, which has no other caller.

### C377. The browse-snapshot refresh is copied six times and redone every frame of a drag
*low - cleanup - effort S*

`RefreshBrowse()` exists (src/Game/MapView.h:201), yet the same line is written out at src/Game/MapView.cpp:301, 734, 811, 860 and 883. Line 883 runs on every frame of a paint drag on a browsed level, and `BrowseLevel` copies the whole map and entity list each time (src/Game/DungeonWorld_Remote.cpp:431-453). This only affects the editor. Fix: call `RefreshBrowse()` at every site, and rebuild only when `EditRevision()` changes.

### C378. Map colours are hard-coded outside MapColors.h
*low - cleanup - effort S*

MapColors.h exists so the map views share one set of colours, but literal colours sit outside it:
- the niche gold (twice) and the bore cyan (src/Game/MapView.cpp:1221, 1232, 1246)
- local selection colours (MapView.cpp:1377)
- corridor and refusal colours (src/Game/MapView_Tools.cpp:266, 281)
- the issue tooltip's own background (src/Game/MapView_Issues.cpp:131)
- WorldMapView's own palette (src/Game/WorldMapView.cpp:22)

The party marker already differs between the two views: `theme.accent` in one, `kParty` in the other. Fix: give these names in MapColors.h, pick one party colour, and draw the issue tooltip on `kMapBg`.

### C382. ContextMenu and MenuList pad with raw pixels, breaking the rem rule
*low - rule - effort S*

ContextMenu works out its width in rem but draws the label at `x + 10` and the "»" marker at `w - 16`. Below about an 18px rem, the marker overlaps the widest group label. In the bare-hand menu that is the Combat or Magic row. At 900p (17px) the overlap is about 1px, and it grows in smaller windows. MenuList's flat-look markers also use raw pixels.

Where:
- src/UI/Controls.cpp:1210-1216 (width, measured in rem)
- src/UI/Controls.cpp:1317-1322 (label and marker, drawn in pixels)
- src/UI/Controls.cpp:1348 (the submenu's label, also drawn in pixels)
- src/UI/Controls.cpp:2186-2187 (MenuList's markers)

Fix: draw at Rem() offsets that match the measurement. Separately, src/UI/FloatingPanel.cpp:18-19 makes its own copies of VK_CONTROL and kPi. Use a new `vk::Control` in Platform/Input.h and Core's kPi instead.

## 11. Core and platform

### C383. Typed text is cut down to one byte instead of encoded as UTF-8, so non-ASCII letters become garbage or act as Enter/Backspace
*high - bug - effort M*

The window is Unicode, so WM_CHAR delivers UTF-16 units. `Input::OnChar` keeps only the low byte (`static_cast<char>(codepoint & 0xFF)`). What goes wrong:
- Accented German, Spanish and Italian letters (u-umlaut, e-acute) become a lone byte, which Font's UTF-8 decoder draws as '?'.
- Russian maps to control bytes, spaces and punctuation, so a Russian player cannot type a party name. The garbage passes `NameValid`, which counts bytes, and is saved.
- U+010D (c-caron) and U+0108 mask to `kTypedEnter` and `kTypedBack`, so typing one in the console submits the line. These letters are outside the shipped locales.

Where: src/Platform/Input.cpp:30, src/Platform/Window.cpp:134, src/UI/Controls.cpp:1618, src/Game/PartyRules.h:126.

Fix:
- In OnChar, combine surrogate pairs, drop control codes by code point, and append the UTF-8 encoding.
- Make the three Backspace sites (TextField, DevConsole, the MapEditor filter) pop a whole character, and make length limits admit or refuse a whole sequence.
- Count characters in NameValid.
- Move `KeyName` (Input.cpp:63) to GetKeyNameTextW plus str::Narrow.
- Add a non-ASCII case to TypingTest.

### C385. diag::Record logs synchronously, so the crash handlers allocate and take locks before writing the minidump
*medium - bug - effort S*

`RecordFor` ends in `LogEvent`. That builds std::format strings, takes the log mutex, and symbolizes a first-seen stack through DbgHelp. So the header's "allocate nothing, take no lock" is false.

As a result the fault and terminate handlers do that work before `WriteDump`, which contradicts their own "record, dump, log last" comments. Both also log the same text twice. Fatal and terminate keep `captureStack = true`, so they symbolize before the dump. The fault path does not.

Where: src/Core/Diagnostics.cpp:404, src/Core/CrashHandler.cpp:99, src/Core/CrashHandler.cpp:150, src/Core/Diagnostics.h:155.

Fix: add `bool log = true` to `diag::Event`. Each crash handler records quietly, dumps, then logs one line (message plus dump status) with the stack last. Update HealthTest.ps1:141 and the header contract.

The verifier rates this a latent risk: the tested crashes already work, and most heap corruption fails fast before any filter runs.

### C386. ThreadManager: `threadreap` can free a Worker the supervisor is reading or restarting
*medium - bug - effort M*

`Manager::Get` returns a raw `Worker*` after releasing `m_mx`. The supervisor then reads every worker, not only Running ones as the header claims.

During a supervisor `Restart`, the worker is Dead and not joinable while `controlMx` is held, from the join until `state.store(Starting)`. A concurrent `threadreap` (Reap takes only `m_mx`) can destroy the locked mutex. The new jthread then captures a dangling pointer, which is heap corruption in the subsystem meant to survive misbehaving threads. The window is narrow and only the dev command can trigger it.

Where: src/Core/ThreadManager.cpp:272, src/Core/ThreadManager.cpp:480, src/Core/ThreadManager.cpp:592, src/Core/ThreadManager.h:171.

Fix: hold Workers by `shared_ptr` and have Get return a copy. At minimum, mark a restarting worker so Reap skips it. Fix the header comment.

### C388. Stack overflow is listed as covered, but the fault filter runs on the exhausted stack
*medium - bug - effort M*

`crash::Install` sets only the filter, the terminate handler and the throw capture. Nothing calls `SetThreadStackGuarantee`.

On a stack overflow the filter runs on the faulting thread with a few KB left. It then does a formatted log write, `MiniDumpWriteDump` and StackWalk64, so it faults again, hits the `g_handling` guard and exits with no dump.

Unbounded recursion is a common crash. CLAUDE.md and CrashHandler.h:8 list it as covered, docs/diagnostics.md:305 names this exact trap, and no test exercises it.

Where: src/Core/CrashHandler.cpp:87, src/Core/CrashHandler.cpp:164.

Fix: call `SetThreadStackGuarantee` (about 64 KB) for the main thread and at the top of `Manager::Run`. Better, hand the dump to a pre-created dump thread. Add `crashpoke overflow` to HealthTest.ps1.

### C391. `-headless` still shows the window, and a saved Borderless or Exclusive mode takes over a monitor
*medium - bug - effort S*

Main creates the window hidden, but the Game constructor calls `ApplyDisplaySettings()` unconditionally:
- **Windowed (the default):** `SetWindowed` uses `SWP_SHOWWINDOW` with no `SWP_NOACTIVATE`, so every headless run shows its window and may take focus.
- **Borderless saved:** a black borderless window covers a monitor.
- **Exclusive saved:** the display switches to fullscreen.

The harnesses run build\debug, which shares settings.ini with Michael's own play. This breaks the "steals no focus" promise.

Where: src/Game/Game.cpp:489, src/Platform/Window.cpp:67, src/Platform/Window.cpp:73, src/Main/Main.cpp:90.

Fix: Window keeps the hidden flag (`IsHidden()`), and the constructor skips ApplyDisplaySettings while it is set. That is the only fix that covers Exclusive, which bypasses Window. As a second layer, drop SWP_SHOWWINDOW and WS_VISIBLE while hidden, and have `Eval.ps1 -SelfTest` require `IsWindowVisible` to stay false.

Windowed centring on the primary monitor is documented (Window.h:55), so it is a separate UX point.

### C392. Alt+F4 is swallowed, and Borderless/Exclusive have no close button, so a load cannot be quit
*medium - bug - effort S*

`WM_SYSKEYDOWN` returns 0 without reaching `DefWindowProcW`, so Alt+F4 never becomes `WM_CLOSE` in any display mode. Borderless is `WS_POPUP`, which has no close box, and Exclusive covers the screen. During every load the console refuses commands, `quit` included. So in Borderless or Exclusive during a load, or a wedged one, the only ways out are the taskbar or Task Manager.

Where: src/Platform/Window.cpp:125, src/Platform/Window.cpp:73, src/Game/Game.cpp:2015, src/Game/DevConsole.cpp:366.

Fix: forward only `VK_F4` to DefWindowProcW. Keep swallowing F10 and a bare Alt, which keeps the window out of menu mode. Exempt `quit`/`exit` from the loading gate, as kNoWorldNeeded already does for the no-world gate.

Against a documented decision: CLAUDE.md says the ways out during a load are "the console and the WINDOW'S OWN CLOSE BUTTON". The console way is false in every mode, and the close button does not exist in two of the three. Fix the code, then the sentence.

### C394. The OBJ loader drops normals for `v//n` faces
*medium - bug - effort S*

For a face entry like "5//3", `sscanf_s("%d/%d/%d")` returns 1 because it fails on the second '/'. The `%d//%d` fallback runs only when the result is below 1, so it is never reached, and the normal stays (0,0,0).

An OBJ exported with normals but no UVs (a common exporter option) therefore imports with zero normals and lights black, with no warning. import-model and the asset dialog's preview both use this loader, nothing regenerates normals, and the bad normals land in a committed .gltf.

Where: src/Assets/ObjLoader.cpp:31, tools/AssetBaker/ModelImport.cpp:63, src/Game/AssetDialog.cpp:414.

Fix: try `%d//%d` first (or test for "//"). Compute flat normals, or warn, when a face's normals resolve to nothing.

### C387. Stalls and kills are recorded with no stack, though `stack::WalkThread` exists for exactly that
*medium - cleanup - effort S*

The supervisor's Stall event and StopOrTerminate's Killed event both pass `captureStack = false`. The kill site's own comment says the wedged thread's walk "has to happen BEFORE this point", but it was never wired in.

`Manager::CaptureStack` / `stack::WalkThread` is safe to call from another thread, yet only `health probe` uses it. AI buckets reboot at 5x a 100 ms watchdog, so the automatic reboot wipes the evidence before anyone can probe. A hang records how long it lasted, but not where the thread was stuck.

Where: src/Core/ThreadManager.cpp:506, src/Core/ThreadManager.cpp:398, src/Core/ThreadManager.cpp:528, src/Core/StackTrace.cpp:118.

Fix: walk the worker on an episode's first Stall and before TerminateThread, and attach the frames. Log them unfiltered as the probe does, since IsPlumbingFrame would drop the OS wait frame that names the lock.

Against a documented decision: CLAUDE.md says Kill records "with no stack - the only stack there belongs to the killer". That was written before WalkThread could read the victim's own stack.

### C384. Path strings mix encodings: ANSI in Paths and file opens, UTF-8 elsewhere
*low - bug - effort M*

Paths.cpp builds strings with `path::string()`, which uses the ANSI code page and throws on characters it cannot map. Elsewhere:
- FileDialog returns UTF-8, and Process::Start widens as UTF-8.
- Assets opens files through narrow `fopen_s`, stbi and cgltf.
- Main.cpp:130 copies wchar_t straight into char.

A non-ASCII profile or install path can throw in `crash::Install`, before the crash handlers are set. A picked folder with an accented name breaks imports.

Where: src/Core/Paths.cpp:21, src/Platform/FileDialog.cpp:55, src/Assets/File.cpp:11, src/Main/Main.cpp:130.

Fix: UTF-8 std::string everywhere, opened through wide APIs, or a UTF-8 activeCodePage manifest.

### C393. The flip-green checkbox can only force the flip on
*low - bug - effort S*

The baker flips green whenever the normal map's name contains "gl" anywhere. The editor only ever sends `--flip-green`, so unticking a wrong guess does nothing.

This is latent: no installed texture has "gl" in its name. The SortTextureDownloads.ps1 token table has drifted from the C++ one, but that is harmless for that script.

Where: src/Assets/PbrMaps.cpp:64, tools/AssetBaker/ImportTextures.cpp:177, src/Game/Game_Editor.cpp:58.

Fix: a tri-state `--flip-green` / `--no-flip-green` that the dialog always sends, and match the GL token only at the end of the name.

### C396. Embedded-image sidecars are keyed by decode-success order
*low - bug - effort S*

The loader names each sidecar by `model->images.size()`, the count loaded so far. A failed decode, or a `data:` URI image, is skipped without being pushed, so every later image picks up the wrong sidecar and a material shows another material's texture. Latent: no shipped model triggers it.

Where: src/Assets/Model.cpp:65, src/Assets/Model.cpp:109, tools/AssetBaker/MipBaker.cpp:120.

Fix: name sidecars by the cgltf image index.

### C398. RestartApp drops `-project`, quits even when the relaunch failed, and lets the child truncate the log
*low - bug - effort S*

The GPU-switch relaunch:
- hardcodes Dungeon.exe and forwards no arguments, so a `-project` run comes back in the wrong world;
- quits even when `Process::Start` fails;
- starts a child that opens `<exe>.log` with "w" while the parent is still shutting down.

It is reachable from the pause menu, and the confirm text does not warn about unsaved progress.

Where: src/Game/Game.cpp:1684, src/Core/Log.cpp:41, src/Game/GameUI.cpp:905.

Fix: rebuild the command line from GetModuleFileNameW plus the original arguments, quit only on success, and have the child wait for the parent before opening the log. Remove the stale `-newgame` comment at Game.h:820.

### C389. Diagnostics ResetEntry leaves the log-throttle fields behind
*low - cleanup - effort S*

When `ResetEntry` hands a slot to a new thread name, it clears the counters but not `windowStartNs`, `windowLogged` or `suppressed`. The new owner can then log "N further events were not logged" for events the previous owner had suppressed. This only happens once all 32 slots have been used by distinct names, so it is rare.

- src/Core/Diagnostics.cpp:154 (ResetEntry)
- src/Core/Diagnostics.cpp:77 (the missed fields)

Fix: clear those three fields too. Merging the AllocTrack, Diagnostics and Profile slot tables into one is optional.

Against a documented decision: the three tables reset a rebooted thread differently on purpose (CLAUDE.md: "A REBOOT DOES NOT CLEAR THE RECORD"). That split is not accidental drift.

### C390. The supervisor thread is unmanaged and allocates every 100 ms
*low - cleanup - effort S*

The supervisor is started directly (src/Core/ThreadManager.cpp:119), not through `Run`. That leaves it with:

- no OS thread name
- no alloc, profile or diag registration
- no try/catch around its work

Each 100 ms poll also builds and reserves a fresh `std::vector<WorkerId>` (ThreadManager.cpp:473). The comments at ThreadManager.cpp:158, src/Core/Diagnostics.h:67 and src/Core/Profile.h:122 all say the supervisor is covered.

These allocations land in the uncounted fallback slot, so no guarded frame is violated. An exception that escapes still reaches the terminate handler's log and dump, but leaves no diag record.

Fix: name and register the thread at the top of `SupervisorLoop`, reuse a member vector for the ids, and wrap the poll in the same catch-and-record that `Run` uses.

### C395. Animator rules are kept by convention, and the asset picker missed one
*low - structure - effort S*

`LockRootTravel` is a separate opt-in call. The world (src/Game/DungeonWorld_Load.cpp:1065) and both editor previews make it, but the asset picker's looping idle (src/Game/AssetPicker.cpp:430) does not. That breaks the rule in DungeonWorld.h:1221, though idle clips drift little.

Separately, re-Playing the current clip mid-fade restarts it (src/Animation/Animator.cpp:46), which contradicts Animator.h:39. Nothing triggers this today, because `DriveMonsterAnim` plays only on a state change.

Fix: make a same-clip Play a no-op while fading, build monster animators through one factory that locks root travel, and drop the unused empty-name default.

### C397. Texture and model load failure handling does not match its comments
*low - cleanup - effort S*

- `LoadTextureFile` is documented as aborting when a file is missing (src/Game/AssetUtil.h:57, and the "dies if absent" comment at src/Game/DungeonWorld_Load.cpp:379), but it returns a magenta placeholder (src/Game/AssetUtil.cpp:177).
- A missing `_n` map therefore renders as a checker (DungeonWorld_Load.cpp:388). Loading it with TryLoad would return null, which the renderer already replaces with a flat normal.
- `LoadTextureThumb` (AssetUtil.cpp:61) still lacks the warning for a rejected .dds.
- `materials[0]` is used unchecked at DungeonWorld_Load.cpp:1979/2022/2045 and Render.cpp:597/640/671.
- Texture.h:9 still says "Always RGBA8".

Every shipped model has a material and every import writes `_n`, so this is latent for now. Fix: rename the function and correct its comments, load `_n` with TryLoad, and have both loaders share one dds-else-png core so they log the same warning.

## 12. Harnesses and tools

### C417. Bc7Test's baseline loader reads nothing, so the quality-regression gate never fires
*high - bug - effort S*

`LoadBaseline` loops on `while (in >> name >> psnr)` and only tests for '#' inside the loop. The baseline file, and every file `--write-baseline` produces, starts with `# BC7 quality baseline - ...`. On that line `name` reads "#", then `psnr` fails on "BC7", which ends the loop and returns an empty map. The comparison never finds a match, so every run reports `regressed=0`, even though Bc7Test.ps1 passes `--baseline` on every normal and CheckAll run.

The header's promise that "a refactor that quietly loses a dB is a failure" has never been checkable. The self-test only corrupts bytes, so it can't notice.

- tools/Bc7Test/Main.cpp:355 (loader), :910 (comparison)
- tools/bc7-baseline.txt:1, tools/Bc7Test.ps1:84

Fix:
- Read with `getline`, skip blank and '#' lines, and parse each line with `istringstream`.
- Print a line for each unmatched image. The real-texture picks change with the installed pool, so a missing entry should not fail.
- FAIL when zero entries match, or when no `syn.*` image matches.
- Add a self-test that raises the loaded values by about 1 dB and requires `regressed > 0`.

### C402. FetchTextures.ps1 reads the dead level1.map, so a fresh clone fetches 2 of the 12 sets the crypts draw
*medium - bug - effort M*

By default the script builds its wanted list only from `textures` records in assets/maps/*.map (tools/FetchTextures.ps1:107). The only file there is the dead level1.map. The game no longer parses that record: levels use `palette` records of catalog ids. Of the 12 sets crypt1/crypt2 draw, only floor_slabs and ceiling_cracked are fetched. The rest render magenta, but the script reports success because its prop sets keep `$imported > 0`. Severity stays medium: this only affects a fresh clone, it shows up visibly (magenta), and `-All` / `-Materials` work around it. The verifier also noted that the other five fetched sets are still used by Test-World.

Fix:
- Build the list from every `texture =` field in the project, library and template catalogs.
- Throw on any wanted name that has no archive folder.
- Delete assets/maps/.
- Fix README.md:58-64, docs/editor-type-authoring.md:346 and the stale "dies if absent" comment at src/Game/DungeonWorld_Load.cpp:379.

Against a documented decision: CLAUDE.md says the script imports what "the maps' `textures` records reference" and still describes level1.map as live. That stopped working when levels moved to `palette` records.

### C406. Worn-block relief and seed come from two disagreeing sources
*medium - bug - effort M*

**The two sources:**
- The full bake uses a per-set table with its own relief and seed, always at wear 1 (tools/AssetBaker/ModelBaker.cpp:2554-2618; for example wall_brick is 0.060 with seed 911).
- `wornblock` falls back to per-kind defaults with a `std::hash` seed (ModelBaker.cpp:2671). The schema defaults (src/Game/CatalogSchema.cpp:95) repeat those per-kind values.

**What goes wrong:** no catalog sets relief or wear. So saving any `rebakes` field in the type editor re-bakes a table set at 0.055 with a new noise seed, and the geometry shifts with no value changed. The slider also shows the wrong starting value. The verifier narrowed two claims: "four authorities" is really two value sources plus an unused field, and the reverse case (`AssetBaker models` or ReplayImports undoing a tuned value) can't happen until some catalog sets one.

**Fix:** keep one record per texture set (kind, relief, seed) and have both bake paths and the type editor read it. Worn meshes are shared by every world, so this belongs to the set, not the type. Fix the comment at src/Game/Game_Wiring.cpp:585.

Against a documented decision: CLAUDE.md's "untouched types bake as before" is false for every set in the table.

### C407. "Use installed" re-bakes a shared worn mesh, even as the wrong surface kind
*medium - bug - effort M*

Creating a surface type from an installed set always runs a `wornblock` bake (src/Game/Game_Wiring.cpp:447). That bake uses the new catalog's kind (src/Game/Game_Editor.cpp:61), but the output file is keyed only by set name (tools/AssetBaker/ModelBaker.cpp:2471), and the game loads it by set name (src/Game/DungeonWorld_Load.cpp:253).

**Example:** make a floor type from a set a wall type already uses. `worn_<set>_*` becomes a floor grid, and every wall of the old type now draws floor geometry. A second type of the same kind still re-bakes at the default values. Nothing in the editor enforces "one set, one surface kind".

**Fix:** skip the bake when `worn_<set>_med.gltf` already exists (src/Game/AssetUtil.cpp:258 already checks for it), and refuse when another type binds the set as a different kind.

Against a documented decision: CLAUDE.md says "Use installed" means "no bake".

### C409. `wear` and `relief` only reach the height-map term
*medium - bug - effort S*

`flat = wearScale <= 0` is only checked for walls (tools/AssetBaker/ModelBaker.cpp:2424). Floors and ceilings always bake the full grid, and their noise ignores relief: about ±1 cm on floors (:958) and ±0.75 cm on ceilings (:973). The procedural wear functions ignore relief entirely, and the wall bow term is unscaled (:941). So wear 0 is not flat on floors or ceilings, and on a surface with no height map the sliders do nothing. That contradicts the schema help, "0 = a flat panel" (src/Game/CatalogSchema.cpp:120). The marble comment at ModelBaker.cpp:2559 is also wrong.

Fix: emit the single flat quad for all three kinds, scale every wear term by relief and wear, and correct the comment.

### C410. `AssetBaker runes` writes only PNGs, and the loader prefers the stale .dds
*medium - bug - effort S*

`runes` writes only PNGs and asks the user to run `mips rune_` afterwards (tools/AssetBaker/Main.cpp:173). `TryLoadTextureFile` loads any .dds that parses, with no age check (src/Game/AssetUtil.cpp:44). Rune .dds files already exist, so after a standalone `runes` run the game quietly keeps drawing the old tablets. That is the "fallback that hides its own firing" trap CLAUDE.md warns about. The full bake is safe.

The "dds at least as new as its source" rule is written three times elsewhere: tools/AssetBaker/MipBaker.cpp:124 and :149, and src/Assets/Model.cpp:66.

Fix:
- Add one `assets::BakedIsCurrent` helper and use it in all four places. The texture loader should warn and decode the PNG when the .dds is stale.
- Have `runes` bake its own mip chains, as `import` does.
- Load with `bakedImages` in `BakeModelImageMips`. Today it decodes every embedded image before the freshness test (MipBaker.cpp:110).

### C418. ThreadStress never sets monster targets, so every monster paths to a wall
*medium - bug - effort S*

`BuildSnapshot` never sets `Agent::targetX/Z`, so they stay at (0,0) (tools/ThreadStress/Main.cpp:113). `Brain::Think` engages toward that target (src/Game/MonsterAI.cpp:161). (0,0) is a border wall here, and `FindPath` only accepts a walkable goal (:229). Every search therefore explores the whole map and fails. The "reachable" phases measure the worst case, and path reconstruction and the pooled path vectors are never exercised.

Monsters also go into `blocked` (Main.cpp:129), while DungeonWorld puts them in the occupancy grid (src/Game/DungeonWorld.cpp:2140). That contradicts the header's "exactly like DungeonWorld" claim.

Fix:
- Target the party's cell.
- Move monsters into the occupancy grid.
- Assert that reachable phases return non-empty paths and unreachable ones return none.
- Derive the bucket thresholds from `Scheduler::BucketForIq`.

### C419. Self-test verdicts accept any failure, so they can't show the injected fault was caught
*medium - bug - effort M*

Several self-tests check only that something failed:

- **RollTest** (tools/RollTest/Main.cpp:2543) passes if any check fails. Its dice fault never reaches the strike sections, because `ResolveAttack` builds its own `RollRules` (src/Game/Combat.cpp:47).
- **ThreadStress** (Main.cpp:472) and **Bc7Test** (Main.cpp:940) use the same one-bit verdict.
- **HealthTest** (tools/HealthTest.ps1:348) passes when any case fails. A case that throws counts as a failure, so a game that crashes at boot passes the self-test. CLAUDE.md's "all 7 cases fail" was a one-time measurement that nothing enforces.
- **InGameTest** renames every sweep label, so real labels are never tested.
- **ProfileTest** skips the rest of its checks under -SelfTest. The script says so, but its header claims more.

TypingTest is fine; the verifier refuted that part of the finding.

Fix: use SpellTest's rule everywhere (tools/SpellTest.py:356), where exactly the injected checks must fail and the rest must pass. For HealthTest, a harness error should fail the self-test.

### C420. DiagTest's log checks pass when the log is unreadable
*medium - bug - effort S*

- **Vacuous check:** `CountLogLines(...) >= 0` (tools/DiagTest/Main.cpp:369) is true whenever the log file opens.
- **Can't be fixed in the test alone:** the "swallowed" summary is only written when a later event opens a new one-second window (src/Core/Diagnostics.cpp:127, :398). A burst followed by thread exit never logs it at all, which is also a product gap.
- **Skip instead of fail:** an unreadable log prints `[skip]`, not FAIL (Main.cpp:77, :338). The log path is a hand copy of src/Core/Log.cpp:42.
- **Mislabelled:** identical-repeat suppressions feed the same counter that is reported as "rate limit".

Fix: expose `log::FilePath()` and treat an unreadable log as FAIL. After the burst, wait past the window and record one more event, then require exactly one summary line with the right count. Alternatively, flush pending suppressions when a thread unregisters.

### C426. No harness detects a stale exe, and `-Only` never builds
*medium - bug - effort S*

No harness checks whether the exe is current. CheckAll's `-Only` (tools/CheckAll.ps1:166) skips the build rows, and /check-alloc, -pipeline, -ingame, -health and -profile all go through it. The Python judges hardcode build\debug\bin\Dungeon.exe (tools/SpellTest.py:39, tools/EditorTest.py:118) and never build. A PASS on yesterday's binary reads as a PASS on today's change. The verifier rated this medium, arguably low: the quick and full tiers do build first, and check-eval.md warns about it.

Fix: have `-Only` run the matching build row first (build-profile for `profile`). Give the standalone harnesses one shared helper that runs the build, or `ninja -n`, and refuses with an exit code distinct from FAIL. Ask the build system rather than comparing timestamps.

### C427. InGameTest's coverage label shows the command ran, not that the screen opened
*medium - bug - effort M*

The game logs `uioverlap [label] ---` as soon as the command runs (src/Game/Game_DevDiagnostics.cpp:516), whether or not the screen opened. That undercuts the header's "coverage is self-verifying" (tools/InGameTest.ps1:19). Only the stair inspector, portrait picker, party page and party builds check that their screen actually opened. A refused generator-tab or new-world-dialog command audits whatever is underneath and passes.

The verdict also matches labels as bare substrings (InGameTest.ps1:386). So `sweep_worlds` is satisfied by `sweep_worldsettings`, and likewise newworld, party3, party1 and stair by their longer siblings.

Fix:
- Match `uioverlap \[<label>\] ---` exactly.
- Have each open step log a status line that the verdict requires.
- Print `state` at the end and require `playing` or `worldmap`.

### C429. HealthTest waits on "Game loaded:", which comes too early
*medium - bug - effort S*

HealthTest waits for "Game loaded: " and then sleeps 2 s (tools/HealthTest.ps1:248). src/Game/Game.cpp:2127 documents that this line lands before the level's own load even begins, while console commands are still gated off. On a cold cache, `crashpoke` and `threadwedge` are refused, and the case fails for a harness reason that the self-test (C419) can't tell apart from a working check. AllocTest (tools/AllocTest.ps1:785), InGameTest and ProfileTest each have a comment rejecting this exact wait.

Fix: wait on `(Level ready: |New game started)`, then confirm the console responds with an echo, through a shared helper.

### C430. Eval.ps1 and the Python judges allow a second run in one worktree and ignore exit codes
*medium - bug - effort S*

The eval runners have three gaps in how they trust a verdict:

- **No single-instance guard.** The game writes one log per exe folder, opened with `fopen(..., "w")` (src/Core/Log.cpp:41). Two runs from one worktree therefore truncate and interleave each other's verdict source. The PowerShell drivers refuse this case, but Eval.ps1 and the Python judges do not.
- **Exit codes are ignored.** The main Eval run never reads `$p.ExitCode` (tools/Eval.ps1:437). EditorTest and WorldTest drop the code, and SpellTest captures it but never reads it.
- **Debug headless still opens a console.** src/Main/Main.cpp:30 calls AllocConsole under _DEBUG before `-headless` is parsed, and every Python judge runs the debug build.

Fix: add one shared "refuse if this worktree's exe is running" check, require exit 0 or the BATCH verdict, and skip AllocConsole under `-headless`. A `-log <file>` switch is optional.

The verifier ruled out the stale-log case (a launch that dies before its first log write) as near-impossible. CLAUDE.md's HEADLESS note ("steals no focus", "can be run several at a time") is currently true only across worktrees and only in release builds.

### C431. EditorTest and WorldTest modify the real project and restore non-atomically
*medium - bug - effort M*

Every EditorTest phase backs up with `rmtree(backup)` then `copytree(PROJ, backup)`, and restores with `rmtree(PROJ)` then `copytree(backup, PROJ)`. Phase 16 does the same to assets/library. This fails in two ways:

- A run killed mid-phase (for example a tool timeout across ~30 launches) leaves dungeon-demo mutated. The next run's first `rmtree(backup)` then throws away the only clean copy.
- A kill during the restore leaves the folder deleted.

WorldTest restores in place, which is safer, but it keeps its originals only in memory. It also downgrades and then deletes worldtrip.dsav and worldpersist.dsav in the shared Documents\DungeonSaves.

Where: tools/EditorTest.py:199, tools/EditorTest.py:1176, tools/WorldTest.py:1054.

Fix: run against scratch worlds with `-project`, like `scratch()` in tools/LevelBuildTest.py:115. Where the real tree must change, overwrite in place and never delete an existing backup. Give harness saves names per worktree. The projects are git-tracked, so what is really at risk is uncommitted edits.

### C435. BuildWallArch's slab has T-junctions and no closed-shell check
*medium - bug - effort M*

The slab is listed by hand and has T-junctions:

- The side panels run the full height at x = +-R (tools/BuildWallArch.py:108), but the spandrel fan only starts at the springline.
- One top quad spans the whole width (131), while the faces below it split at +-R and at every arc segment.

`remove_doubles` welds vertex to vertex, never a vertex into an edge, so these seams never close. The script then recalcs normals over the open shell (182) and bevels every edge. The verifier counted 300 open edges in each committed model (wall_arch_rustic and wall_arch_rough), against none in the hand-built wall_arch. Expect a crack or notch above each jamb. No level places these decorations yet.

BuildDoorFrame.py:131 diagnosed this exact defect and fixed it with a solidity grid plus an `n_boundary == 0` assert (404). Do the same here, and make that assert a shared helper.

### C442. Declined commands Print instead of Refuse, so eval scripts never count them
*medium - bug - effort M*

src/Game/Game_Eval.cpp:343 fails a script only on `ConsumeRefusal()` or an unknown command. Many commands decline with `Print` instead. A script whose setup line was declined then measures a world it never built, and the run still passes (eval-audit F11/F15).

- **Declines that only Print:** `load`, `goto`, `press`, `opendoor`, `generate`, `stairadd`, `levelrename`, `give`/`equip` "no item", `effect`, `torch`, `setsupply`, `worldpos`, the terrain brush, half of `typeset`, ParseSymbolArg.
- **False success:** `editor move` and `erase` always report success (src/Game/Game_DevCommands.cpp:303).
- **Unknown verbs:** an unknown `editor` verb prints "map: editor mode" (658), and an unknown `profile` verb expands the panel.
- **Drifted usage:** hand-written usage lines no longer match the registered params (`worlds`, src/Game/Game_DevWorld.cpp:531).

Fix:
1. Add an `expect-refuse <line>` directive first. dungeondelete.eval, worldprops.eval and dungeonrefuse.eval probe refusals on purpose, and they only pass today because those paths Print.
2. Convert the declines to Refuse.
3. Move arity errors onto devargs::Need.

### C444. `rest until` ignores StepStop and mislabels stops
*medium - bug - effort S*

`rest until` never reads the `StepStop` that StepWorld fills in (src/Game/Game_DevParty.cpp:1295-1308). If the app is not Playing (for example, the party just wiped), StepWorld returns 0 and SetResting never refuses. The command then prints "rested 0.00s - still resting (hit the cap)" instead of refusing. LevelChange and Ceiling stops are also labelled "hit the cap".

This happens in the committed expedition.eval: rung 4 wipes the party and is followed by `rest until 900`. `step` already refuses in this case (src/Game/Game_DevEval.cpp:461).

Fix: refuse on NotPlaying before entering rest, name each stop reason, and use `kStepTicksPerSecond` instead of the hard-coded 60.0f, ideally through one helper shared with `step`. The verifier dropped a claimed lingering 60x rest: `heal` ends the rest as "recovered", and reset clears it.

### C399. ModelBaker.cpp is 2,677 lines, and its two worn-relief defaults disagree
*medium - structure - effort M*

The file is past the ~2000-line split rule. It mixes primitives, blocks, props, doors and creatures, has three near-identical worn-block builders (tools/AssetBaker/ModelBaker.cpp:994-1102), and carries ~70 lines of RETIRED notes.

The size is not the main problem. Two relief defaults disagree:

- `AssetBaker models` bakes from the hard-coded specs[] table (2554), which has a relief per set (wall_cobble_round 0.075) and fixed seeds.
- An editor `wornblock` rebake with no catalog `relief` uses the per-kind 0.055/0.045/0.08 and a hashed seed (2666).

So changing only `wear` in the editor rebakes a tuned set shallower and with different noise. The next `models` run then silently reverts it.

Fix: give each set one relief/seed default and have both paths read it. Then split the file, keeping each tombstone as a one-line "do not bake here" guard.

Against a documented decision: CLAUDE.md's "untouched types bake as before" is false for sets specs[] tunes. The comment at src/Game/Game_Wiring.cpp:586 is wrong for the same reason.

### C400. RollTest main() is one ~2,450-line function
*medium - structure - effort M*

tools/RollTest/Main.cpp is 2,549 lines. `main()` runs from line 97 to the end and holds about 25 brace-block suites (dice, strike, armor, blasts, resources, ledger, power, carve, party rules, light tiles...). Only `--self-test` is parsed, so you cannot run one suite on its own.

Smaller problems:
- The file header still describes only dice and armor.
- The `fields` lambda is copied at lines 2316 and 2395.
- An inner `kSamples = 20000` (2350) shadows the outer one (102).

Fix: split it into one file per concern, each a `RunX(Harness&)`, with a shared Check/CheckTrue/fields header. `main()` then just dispatches the suites (optionally filtered by name) and prints the verdict.

### C401. Harnesses copy their launch, input and wait code, and the copies drift
*medium - cleanup - effort M*

Each of AllocTest, InGameTest, HealthTest, ProfileTest and TypingTest has its own PostMessage class, Send-Key, Send-Text and Wait-ForLog. Fixes have reached only some of the copies:

- **Window lookup:** all five use `$proc.MainWindowHandle`. The reference driver, docs/drive.ps1:40, finds the window by PID plus window class instead, because a debug build also owns a console window. AllocTest (tools/AllocTest.ps1:755) and TypingTest also skip the 30 s retry the others have.
- **Starting a game:** TypingTest (tools/TypingTest.ps1:173) still presses Enter on the title screen, which means Continue on the newest shared save. It recovers, but types into an arbitrary save.
- **Python run():** EditorTest (tools/EditorTest.py:132) and WorldTest drop the return code and let TimeoutExpired escape.

Fix: create tools/HarnessGame.ps1, dot-sourced like HarnessAudio.ps1 (own-game start with window lookup and retry, newgame via the console, log waits, stop by PID), plus a matching harness_game.py.

### C411. TextureBaker.cpp is entirely dead
*medium - cleanup - effort S*

BakeTextures (tools/AssetBaker/TextureBaker.cpp:174) writes bare files like `wall_brick.png` and `wall_brick_n.png`, with no resolution suffix and no `_mr`. Nothing reads them:

- LoadPbrSet only tries `<name>_<res>`, then `_2k` (src/Game/DungeonWorld_Load.cpp:370).
- InstalledTextureSets rejects stems without a resolution tag (src/Game/AssetUtil.cpp:220).
- The worn bake reads only the `_1k/_2k/_4k` normal maps.

The full bake still runs it (tools/AssetBaker/Main.cpp:198) and BC7-encodes the output. The names also collide with the scanned sets, which invites fixing a texture in the wrong place.

Fix: delete TextureBaker.cpp/.h, its CMake entry and the call. Keep Noise.h, which ModelBaker and RuneBaker use.

### C437. Model scripts never bake the embedded-image sidecars
*medium - rule - effort S*

tools/FetchModels.ps1 and tools/FetchAnimLibrary.ps1 (line 232) produce models with embedded images but never run `AssetBaker model-images`. The comment at FetchModels.ps1:373 says no mips pass is needed, which is out of date for embedded images.

When a sidecar is missing, src/Assets/Model.cpp:68 decodes the images silently; only a stale sidecar warns. Each model then pays the CPU decode that CLAUDE.md measured at ~320 ms for skel_warrior (against ~50 ms with sidecars), and nothing reports it. CLAUDE.md's "regenerable alternative" list also leaves out FetchAnimLibrary, which is the only source of the gitignored skel_* models.

Fix: end both scripts with `model-images`, log a missing sidecar once per model, and add FetchAnimLibrary.ps1 to CLAUDE.md's list.

### C408. Height-map resolution fallback can discard the texture's aspect
*low - bug - effort S*

The worn bake's height-map loop (tools/AssetBaker/ModelBaker.cpp:2429) can replace a flat but correctly sized `_2k` map with a missing `_4k` one, which drops Aspect() to 1. An editor-imported non-square set with no height map would then get squashed worn UVs. No shipped set triggers it. Fix: take the aspect from the first sized image, or from the albedo as the game does.

### C414. Mip chains are box-filtered in gamma space with truncating division
*low - bug - effort S*

`Downsample` (src/Assets/Image.cpp:52) averages raw bytes with a truncating `sum / 4` for every map type (tools/AssetBaker/MipBaker.cpp:76). Averaging in gamma space darkens fine albedo detail at distance. The truncation also loses about 4 codes over a 2k chain, which lowers parallax height too. Fix: add an explicit `srgb` flag and round with `(sum + 2) / 4`.

### C424. RollTest indexes parser results without checking their size
*low - bug - effort S*

tools/RollTest/Main.cpp:2034 and :2036 index `ParseMonsters(...)[0]` without a check. Line :2104 does the same with `t1.rows[0][0]`. The checks at :2026-2029 guard with a size check first. If the parser regresses to an empty result, a debug run hits the CRT subscript assert and hangs on a dialog instead of printing FAIL. Add the same size guard. Also, :1522 has a no-op `sealed ? fire : fire`.

### C428. The "console ready" retry loops are satisfied by an earlier echo
*low - bug - effort S*

After `newgame`, two loops wait for the console to answer:
- tools/AllocTest.ps1:813 searches the whole log for `console: > logecho on`.
- tools/InGameTest.ps1:323 does the same for `> logecho off`.

Both lines were already written on the title screen (AllocTest.ps1:771, InGameTest.ps1:298), so the first try always succeeds and the guard never waits. The risk is small: both loops already run after a wait for the level, and both harnesses now start with `newgame`. Fix: count only new lines (Wait-NewLogLines), or echo a unique token on each try.

### C432. WorldTest and LevelBuildTest have checks that cannot fail
*low - bug - effort S*

- tools/WorldTest.py:376 ends in `... or True`, so it can never fail. Make the control real or delete it.
- tools/LevelBuildTest.py:312: a phase number outside 1-8 runs nothing and still prints PASS. Refuse unknown phases and require at least one check.
- In WorldTest phase 16 (:735-741), the settings.ini control is set up only when the file existed before the run, but its check still prints. Create the control when the file is missing.

The verifier found the original finding's EditorTest phase 18 claim wrong, so it is left out.

### C434. ConvertMesh --keep-rig reads Action.fcurves, which Blender 5.x removed
*low - bug - effort S*

`is_skeletal` at tools/ConvertMesh.py:479 reads `action.fcurves` directly. The `all_fcurves` shim in the same file (:196), and its copy in tools/ImportAnimLibrary.py:318, exist because that attribute is gone in Blender 5.x. The function only runs when two actions share a base name. The centipede and spider conversions succeeded without reaching it, so this is a latent AttributeError, not a current failure. Because FetchModels.ps1 runs Blender without --python-exit-code, the error would show up as "No rigged glb produced". Fix: call `all_fcurves(action)` inside `is_skeletal`.

### C436. An atan2 UV seam reverses one face column on the fountain, cork and rock
*low - bug - effort S*

At tools/BuildFountain.py:182, u comes from atan2, which jumps from +pi to -pi at the back of the shape. One face column therefore runs u backwards around the whole circumference: about 8.9 texture repeats, measured in fountain_round.gltf. The cork (tools/BuildPotion.py:242) and the rock (tools/BuildRock.py:92) have the same seam, but it is much less visible. tools/BuildPillar.py:95 avoids the problem by taking u from the unwrapped segment index. Use that method here too. fountain_round is not placed in any level today.

### C441. levelcheck checks only the model field, matched by file stem
*low - bug - effort S*

levelcheck (src/Game/Game_DevWorld.cpp:687) compares each entry's `model` field against file stems (src/Game/AssetUtil.cpp:313). That misses several cases that each end in a LoadModelOrDie abort:
- a .glb vs .gltf extension mismatch
- the id fallback for entries with no `model`
- `empty_model` and `part2_model`
- worn_<set>_<tier> meshes

The verifier found that no current content hits any of these, and the gitignored-model failures the guard was written for are caught. Fix: resolve models the way the loaders do, cover every model field, and check the worn meshes for every palette.

### C443. StepWorld keeps ticking after the party is wiped mid-step
*low - bug - effort S*

StepWorld (src/Game/Game_Eval.cpp:109) stops only on a level change or the end of a rest. A wipe inside Update sends the game back to the title (src/Game/Game.cpp:1545), but the loop keeps ticking anyway. `step` then prints a full-length run with no stop reason, and the TALLY seconds include time after the party fell. The verifier notes that a wipe during a sweep is a measured outcome by design (`downed=` records it, `heal` recovers), so only the report is wrong. Fix: add a StepStop::PartyWiped, break when the game is no longer Playing, and have `step` print the reason.

### C445. An unreadable script in the middle of a batch repeats the previous verdict
*low - bug - effort S*

When the next script in an `-eval` batch can't be read, LoadEvalScript clears the lines before it fails (src/Game/Game_Eval.cpp:169). The runner then returns (:247) still holding the previous script's state. On the next frame it logs a second `RESULT=... script=A lines=0` and counts script A again. The batch result is FAIL either way, so only the `scripts=` and `failed=` counts are wrong. Fix: in the same frame, keep trying pending scripts until one loads.

### C446. setskill accepts any skill id
*low - bug - effort S*

`setskill` (src/Game/Game_DevParty.cpp:1460) writes `skillXp[args[1]]` without checking the id and reports success. A typo such as `fyre` adds a key that nothing reads, and the test then measures an untrained caster. BuildMember (src/Game/Game_Party.cpp:144) already refuses ids that are not in TrainableSkills. Do the same check here. Every id the scripts use today is trainable, so the check breaks nothing.

### C447. The console throw command loses the held item's charge
*low - bug - effort S*

The console `throw` (src/Game/Game_DevParty.cpp:242) calls ThrowItem without passing the held item's charge, so a part-burnt torch lands as a fresh one. The mouse path (src/Game/Game.cpp:2575) and the hand-menu path (src/Game/Game_Wiring.cpp:231) both pass `m_heldItem.Charge()`. Pass it here too, as the `torch mount` branch in the same file already does.

### C448. hudpanel writes settings directly instead of going through GameUI
*low - bug - effort S*

`hudpanel` place and lock (src/Game/Game_DevParty.cpp:1087-1104) write m_settings and save it without going through GameUI. Because of that:
- m_hudSlidersStale is never set (src/Game/GameUI.cpp:1923), so the Settings scale slider and lock checkbox show old values. The panel itself does not snap back.
- hide and show skip OnHudPanelHidden.
- The refusal list at :1108 still names the removed `options` panel.

Fix: route these through GameUI methods, and build the panel lists from kHudPanelFields.

### C449. editor place picks a default wall from the wrong map and ignores taken faces
*low - bug - effort S*

When no face is named, `editor place` takes the first solid face from the active map (src/Game/Game_DevCommands.cpp:594). The brush, however, edits the level being viewed (src/Game/MapEditor.cpp:746). The scan also ignores faces already in use, unlike DungeonMap::FreeSconceWall (src/Game/DungeonMap.cpp:881). Every wrong face ends in a counted refusal, never a misplaced object. Fix: choose the face with the viewed map's FreeSconceWall or FreeNicheWall.

### C450. allocguard's "steady (armed)" line can never print
*low - bug - effort S*

`allocguard` can never report the armed state. `SteadyStateFrame` runs first in `Game::Update` and sets `m_steadyFrames` to zero whenever the console is open or a script is running. A command can only run in one of those two cases, so it always prints "settling (0 quiet frames)". The `120` also repeats the function-local `kWarmupFrames`.

- src/Game/Game_DevDiagnostics.cpp:189
- src/Game/Game.cpp:1744

Fix: move `kWarmupFrames` up to a Game constant. Then either report the last quiet streak from before the console opened, or drop the line.

### C403. Level-generator scripts write levels that no longer exist; unused icons; stale tool comments
*low - cleanup - effort S*

- tools/BuildShowcase.py:190 writes `showcase.map/.ent` into dungeon-demo. project.ini lists only `crypt1 crypt2 eval_arena`, so `goto showcase` refuses it. eval_arena replaced the showcase, so delete the script.
- tools/BuildGallery.py:219 defaults to `start level2`, and neither level exists. It also never adds its levels to the manifest. The tool is still useful, so make `--levels` required.
- assets/ui/icon_cast.png and icon_clear.png have had no loader since 9e5c7cae.
- Comments that point at things that are gone or changed:
  - tools/FixArchSoffitUv.py:67 cites RoughenArch.py, which does not exist.
  - tools/bsend.py:8 cites BuildArch.py, which does not exist.
  - tools/WorldTest.py:364 still reasons about showcase and level2.
  - tools/CutBarFrame.py:6 and CLAUDE.md's RESOURCE BARS describe the red Life bar, but the script's `DEFAULT_SRC` is "Mana Status Bars (13)".

The verifier noted the LF line endings are harmless, because .gitattributes normalises them.

### C404. Blender build scripts carry private helper copies, and one has drifted
*low - cleanup - effort M*

- The same `box()` with the sorted-bounds winding fix appears in tools/BuildDoorPad.py:82 and tools/BuildLever.py:95. tools/BuildStatue.py:295 has the same helper without the fix. Its current calls happen to be ordered, so nothing is wound wrong today.
- `face()` and the voxel-surface loop are verbatim in BuildDoorFrame.py:169, BuildDoorLeaf.py:270 and BuildStoneSlab.py:205. Those three scripts weld and recalculate normals, so the winding argument matters less for them.

Fix: at minimum, port the sort fix into BuildStatue. A small shared `tools/blendlib.py` would stop this happening again, with one trade-off: changing a helper would silently reshape every asset on its next re-run, which strains "the script is the asset". Keep any shared module small.

### C405. Archive-root and Blender-discovery code is copied per script, and the copies have drifted
*low - cleanup - effort S*

- tools/BuildUiStones.py:39 builds the archive path from %USERPROFILE%\OneDrive and ignores %OneDrive%. Every other tool honours %OneDrive%. On a business or renamed OneDrive, every stone prints "NO ALBEDO ... skipped" while the other tools work.
- The Blender search in FetchModels.ps1:45 and FetchAnimLibrary.ps1:44 is identical. The copy in tools/blender-bridge.ps1:17 differs in three ways:
  - its regex is not anchored;
  - its `[version]` cast throws on a folder name with a suffix;
  - it picks the newest folder before checking that blender.exe is there.

Fix: add `tools/pipeline_common.py` and a dot-sourced `tools/Pipeline.ps1` (archive root, Find-Blender), and point every script at them.

### C412. AssetBaker re-implements image and mesh helpers per file, and rounding differs between copies
*low - cleanup - effort S*

The copies:
- Four bilinear samplers: tools/AssetBaker/ImportTextures.cpp:78 and :92 clamp; ModelBaker.cpp:775 and RuneBaker.cpp:157 wrap.
- Three PNG writers.
- `AddFace` duplicates `AddQuad`, and `Smoothstep` duplicates `PinRamp`.
- `kRefSquare` is defined twice.
- The three-mip-chain bake is repeated in Main.cpp:77 and ModelImport.cpp:191.
- The rune stroke table is copied by hand into tools/BuildRuneIcons.py:37.

One copy has already drifted: opacity rounds with `+0.5f` (ImportTextures.cpp:159) but ORM truncates (248-250). That makes imported roughness and AO half a code value low.

Fix: add a baker-local ImageOps.h (a sampler with clamp and wrap modes, plus SavePng) and one `BakeSetMips`. Have RuneBaker emit the stroke table for the Python script to read.

### C413. Unused bake outputs and constants; the "unused" clean wall block is the editor's swatch
*low - cleanup - effort S*

- tools/AssetBaker/ModelBaker.cpp:2623 bakes `sconce.gltf` and `brazier.gltf` on every run, but every fixtures.cat binds `wall_torch` and `brazier_bowl`. Their only use is as the id fallback in src/Game/DungeonWorld_Load.cpp:35.
- `kLeafHalfT`, `kPullHalfH` and `kPullHalfW` (2171-2175) are never used, although a comment at 2207 says they are "still live". `BuildDoorPanel` hardcodes 0.85 and 2.1 (2193) instead of using `kLeafHalfW` and `kLeafH`.
- The comment at 363 says the clean block set is unused. In fact `wall_block.gltf` is the wall-texture swatch in src/Game/AssetDialog.cpp:27, AssetPicker.cpp:394 and Game_Wiring.cpp:30. So the editor previews every wall texture on a recessed panel with edge pillars, which no wall in the game has any more.

Fix: give the swatch a plain full-cell panel. CLAUDE.md's condition for removing AddWallPillars is widening that panel first. Also fix CLAUDE.md's "baked but unused".

### C415. Stale comments in AssetBaker
*low - cleanup - effort S*

Several comments say the opposite of what the code does:
- tools/AssetBaker/Main.cpp:11 and ImportTextures.h:9 say AO is baked into the albedo. AO goes to the `_mr` ORM file.
- MipBaker.h:7 says it writes RGBA8. It writes BC7.
- Bc7Encoder.cpp:2 and Bc7Encoder.h:90 list three modes and a bounding-box score. There are four modes (mode 3 is missing from the lists), and the default score is Scatter.
- ModelBaker.cpp:2546 says palette order matters. Palettes are catalog ids now.
- ModelBaker.cpp:1511 describes a 7-joint rig. The rig has 15 joints.
- RuneBaker.cpp:15 and RuneBaker.h:8 say icons are baked from "four" sets. Icons are no longer baked, and there are ten sets.
- TextureBaker.cpp:33 cites a title baker that no longer exists.

Fix them all in one pass, along with CLAUDE.md's `AssetBaker <assets>` line, which still lists title art and portraits.

### C416. GltfWriter reports success on a failed write; other baker errors are dropped
*low - rule - effort S*

tools/AssetBaker/GltfWriter.cpp:259 ignores the `fwrite` result and returns true. A full disk, or a file the game has locked, leaves a truncated .gltf that later aborts a level load. `assets::WriteBinaryFile` (src/Assets/File.cpp:25) already checks the write and creates directories.

Other dropped errors:
- Names are written into the JSON unescaped (GltfWriter.cpp:152).
- SoundBaker.cpp:53 ignores the frame count `drwav_write_pcm_frames` returns.
- ImportTextures.cpp:232 drops a found map's load error without a log line.

The verifier says the RAII point is weak: nothing leaks today. The unchecked writes are the real problem.

### C421. AnimTest never samples a looping wrap or root-locked playback
*low - cleanup - effort S*

tools/AnimTest/Main.cpp:127 never calls `LockRootTravel`, and no sample crosses a loop wrap. In the game, every monster animator locks the root (src/Game/DungeonWorld_Load.cpp:1065, plus both editor previews).

The verifier narrowed the risk:
- First and last keys are already sampled, at t=0 and t=duration.
- The lock and the wrap are Animator logic, and the `rootmotion` eval already measures them end to end.

So this is a cheap addition: a second pass with `LockRootTravel(0.08f)` that samples looping clips to 1.5x their duration. A nit as well: the `allocs` column (118) prints 0 when tracking is off.

### C422. Bc7Test labels a non-default row "the default"; the GPU-proof comments are stale
*low - cleanup - effort S*

tools/Bc7Test/Main.cpp:401, 404 and 467 label the shapes=16 rows "the default", but `Bc7Options` ships `shapeTrials = 8` (tools/AssetBaker/Bc7Encoder.h:53). CLAUDE.md says these defaults were set by `--audit`, so a mislabelled baseline invites re-tuning against the wrong row.

tools/Bc7Test/Bc7Decode.h:14 and tools/AssetBaker/Bc7Tables.h:11 claim the GPU already drew mode 1 and mode 6 blocks. They were written while the DDS reader was rejecting every file, so no BC7 block had reached the screen.

Fix: build the default rows from `Bc7Options{}`, changing only `threads`. Update the comments, and add mode 3 to the list in Bc7Encoder.h:90.

### C423. RollTest retypes Balance defaults by hand, and one row has drifted
*low - cleanup - effort M*

The strike table row "a monster (60) vs a party member" still uses `25.0f + CurveValue(...)` (tools/RollTest/Main.cpp:312). Real defense is 45 (src/Game/Balance.h:132), and RollTest itself uses 45 at line 347. Many other knobs are retyped too.

The stance defaults exist three times:
- src/Game/Balance.h:289
- src/Game/Defense.h:96
- RollTest:994

The file explains why it retypes them: it cannot link Balance.cpp. The fix answers that while keeping expectations hand-derived:
- move the knob defaults into a pure constexpr header;
- use `defense::StanceRules{}` at line 994;
- fix or delete the base-25 row.

### C425. RollTest and Bc7Test don't use the shared verdict line; three near-identical Check helpers
*low - cleanup - effort S*

DiagTest, ThreadStress, SpellTest and CheckAll already share the `<tool> RESULT=X checks=N failures=M` line. Two tools print their own format instead:
- RollTest prints "PASS ... n checks, m failed" (tools/RollTest/Main.cpp:2537).
- Bc7Test prints `BC7TEST VERDICT=` (tools/Bc7Test/Main.cpp:935).

Three Check helpers are near-identical: RollTest:68, tools/DiagTest/Main.cpp:36 and tools/ThreadStress/Main.cpp:57.

The verifier rejected the rest of the original claim:
- DiagTest's threaded checks are safe, because each thread is joined before the next check.
- CheckAll reads exit codes by design, not because the formats forced it.

Fix: bring RollTest's (and optionally Bc7Test's) final line into the shared convention. A tiny shared Check header is optional; a new framework is not needed.

### C433. Harness muting and -Config only follow CheckAll's own config
*low - cleanup - effort S*

`Invoke-Muted` mutes only `build\<Config>\bin`, then sets a global `DN_HARNESS_MUTED` flag. Two harnesses lose out because of this:

- **ProfileTest.** It runs as release-profile. It sees the flag and skips its own mute, so in `-Full` it plays at your normal volume.
- **SpellTest.** It hardcodes the debug exe and never imports `harness_audio`, so it is never muted.

Refs: tools/CheckAll.ps1:43, tools/CheckAll.ps1:143, tools/ProfileTest.ps1:49, tools/SpellTest.py:39.

**Fix:** set the mute flag per bin directory, and add `harness_audio` to SpellTest.

### C438. ReplayImports.ps1 runs the baker differently from the other scripts and the editor
*low - cleanup - effort S*

ReplayImports calls AssetBaker directly with `$ErrorActionPreference = "Stop"`. FetchTextures warns about exactly this: under PS 5.1 a harmless baker warning on stderr can abort the whole batch. The replay also leaves out `--wear` and `--relief`, which the editor's worn-block bake passes. A replayed surface with authored relief or wear would bake at the defaults and overwrite the committed worn_*.gltf.

This can't happen today: imports.cat is empty and no catalog sets relief or wear.

Refs: tools/ReplayImports.ps1:123, tools/ReplayImports.ps1:133, tools/FetchTextures.ps1:47, src/Game/Game_Editor.cpp:69.

**Fix:** use the shared `Invoke-Baker` wrapper, and pass relief and wear, either read from the catalog or recorded in imports.cat.

### C439. BuildTemplate.py hard-codes a stale start_items list
*low - cleanup - effort S*

tools/BuildTemplate.py:55 writes the template's `start_items` by hand. The list is missing `fire_flask`, which assets/projects/dungeon-demo/project.ini:31 has. The template's items.cat includes fire_flask now, so the copy is just out of date.

Verifier narrowing: the duplicated "fields that point at places" rule is not a risk. `ClearPlaces` also runs on blank worlds (src/Game/Game_NewWorld.cpp:241), and EditorTest already validates a blank world.

**Fix:** read `start_items` (and default_sconce/brazier) from the source project.ini, or note why fire_flask is left out.

### C440. Three Build scripts call their metre-to-unit factor KUNIT
*low - cleanup - effort S*

tools/BuildStatue.py:54, tools/BuildLever.py:37 and tools/BuildDoorPad.py:43 divide by `KUNIT = 2.5`, with comments pointing at `kUnit` in DungeonMap.h. It is really an authoring reference, which the C++ bakers call `kRefSquare` (tools/AssetBaker/ModelBaker.cpp:291). If kUnit changes and someone "syncs" these values and re-runs the scripts, the statue, lever and door pad change size in units. That breaks the no-rebake invariant.

**Fix:** rename it `REF_SQUARE` and copy ModelBaker's "never sync it" comment.

## 13. Documentation drift

### C451. Effect-pipeline comments and design-doc status lines still describe the system before it was built
*medium - cleanup - effort M*

Several docs say this system is unbuilt, and many headers describe the old design:

- docs/effects.md:3 says "PLAN ... Nothing built yet", but its own :553 says all six phases landed. docs/health-and-healing.md:3 says "PART BUILT" and CLAUDE.md:367 says "still design", yet both describe it as complete.
- docs/magic system.md (:296-329) is the "Start here" doc, but it names SpellEffect::Shield, StatusKind::Sight, onFizzle and DungeonWorld::RuneGlow. None of these exist.
- src/Game/Effect/Effect.h:19, WardEffect.h:15 and DotEffect.h:13 say "until P2". The overrides already exist.
- **Real constraint:** Effect.h:423 claims kMaxEffects (16) has headroom. It has none: 4 wards + 3 DoTs + 2 supply effects + 4 sights + 3 lights = 16. The next effect kind can evict one.
- Other stale comments:
  - DungeonWorld.h:3582 is orphaned.
  - DungeonWorld.h:3446 says bolts route through ForEachBreakableAt. They do not.
  - DouseFixture says a dropped torch goes out, which contradicts docs/torches-and-fire.md:42.
  - Stoneskin.h names Character::Armor(), which no longer exists.
  - Spells.h:30 and Magic.h:28 describe the includes wrongly.

**Fix:** rewrite these headers and status lines. Recount kMaxEffects and either raise it or guard it with a static_assert. Decide whether to keep the unused StatBonus/SpeedScale hook.

A verifier narrowed one claim. StaminaSoothe is a party-wide multiplier, so it does not go around that hook.

### C454. docs/ai.md and the AI headers describe a two-mode AI
*medium - cleanup - effort S*

docs/ai.md:82-87 ("Current implementation") says the only intents are Idle and Engage, with no flee, patrol, ranged, cast or leash. Its own phasing (:306-351) marks all of those DONE. src/Game/MonsterAI.h:169 has four modes and :84 has six archetypes. MonsterAI.h:11 and CLAUDE.md's threading section also say idle/engage only. Decision 5's call-for-help is not built: ProvokeMonster wakes only one monster (src/Game/DungeonWorld.cpp:2236). The doc does not list it as a gap.

Two schema help strings mislead authors:
- **offense** (src/Game/CatalogSchema.cpp:266) promises per-archetype defaults. The value is set per kind, with a default of 1.0.
- **faces** (:299) says it only hides the editor arrow. It also removes the monster's blind spot and stops it turning (DungeonWorld.cpp:2151, :1284).

tools/ThreadStress/Main.cpp:129 hard-blocks every monster cell, unlike the game's `occ` grid, yet its header claims faithful wiring.

**Fix:** rewrite the "Current implementation" section as built, list call-for-help as not built, fix the two help strings, and make ThreadStress fill `occ`. Targeting is random until a member crosses the threat threshold, so "random member" is incomplete rather than wrong.

### C455. CLAUDE.md's dialog-chrome rules name helpers that were deleted
*medium - cleanup - effort S*

CLAUDE.md (about :2887-2910) tells dialogs to use ui::kDialogTitleBandH, ui::DialogTitleBand, ui::FitDialogTitle and the TypeEditorDialog/LevelSettingsDialog TitleFont members. None of these exist. Dialogs now get their card from game::BuildDialogChrome / EditableTitle (src/Game/DialogLayout.h:4). 16 dialog TUs use it, and CLAUDE.md mentions it once.

- **Lost protection:** EditableTitle draws the title untrimmed (DialogLayout.cpp:52-61). The shrink-then-ellipsis behaviour the doc promises is gone, so long titles overrun.
- The close-box paragraph (about :2957-2975) cites the panel-rect AddCloseButton overload. That overload and CloseButtonRect (src/UI/Controls.cpp:2471-2484) have no callers.
- **Dead code:**
  - DialogTitleFont / DialogTextFont (Controls.h:1111, 1116).
  - The kLabelW / kFieldX / kFieldW constants at TypeEditorDialog.cpp:29.
  - InstanceInspector::FormRow, which only forwards to game::FormRow.
- **Wrong counts:** "six subclasses" should be seven, and "ten categories" should be 24. TextureSet/Model fields are called dropdowns, but they open the asset picker.
- **Smaller comment drift:** Controls.h:18-21 says fixed pixels, SlotList says rowHeight is in pixels (it is rem), and Font::SetHeight has no caller.

**Fix:** replace both paragraphs with a short note on BuildDialogChrome, delete the dead helpers, and decide whether EditableTitle should trim.

### C456. CLAUDE.md's settings-page section teaches the removed Flow helper
*medium - cleanup - effort S*

CLAUDE.md:1068-1088 says the settings page is authored in design px and scaled by `uiScale`, with rows placed by a Flow helper (mTight/mRow/mGroup). It states a rule: "any new settings geometry must scale by uiScale too". None of that exists. Tabs are window fractions, and each tab is a SettingsTab() Stack in rem (src/Game/GameUI.cpp:99-126, BuildSettings at :404). There are now six tabs, including Material. Following the rule would put pixel layout back into a rem page.

Other drift:
- "Five UIContexts" (CLAUDE.md:166, GameUI.h:4): there are seven.
- "Frozen scene" (GameUI.cpp:1142, 2458): the sheet is not a pause.
- ui-hierarchy.md:358 says nothing hand-places rows, but OpenConfirm (GameUI.cpp:1430) does.
- GameUI_Items.cpp:643 says "a shorter id never allocates". The waterskin -> waterskin_half id is longer and is safe only because it fits the small-string buffer.

**Fix:** rewrite the paragraph around SettingsTab and the kSet* constants, and correct the counts and comments. ARCHITECTURE.md's TextOutput entry is accurate, but TextOutput itself is dead code.

### C458. ARCHITECTURE.md's module table, rules and frame flow are out of date
*medium - cleanup - effort M*

- **Build** (docs/ARCHITECTURE.md:244) says assets are copied beside the exe. They are not: the path is baked in as DN_ASSETS_DIR (src/Core/CMakeLists.txt:28). Both this doc and CLAUDE.md:9 omit the profile configs that build.cmd:3 accepts.
- **Frame flow** (:65-69) names Game::RenderShadowMaps / RenderScene and UI::Context::Render. These are DungeonWorld methods called from Game::Render (src/Game/Game.cpp:2772-2786). The HDR PostProcess pass (bloom, ACES) appears in no doc.
- **Module table:**
  - Core's "event dispatch" does not exist.
  - ThreadManager, Diagnostics, CrashHandler, AllocTrack and Profile are missing.
  - UI lists 5 of about 20 widgets.
  - Animation clips live in Assets/Model.h.
- **Rule 3** ("engine knows no gameplay") is broken by BarKind Health/Stamina/Mana/Food/Water (src/Graphics/SpriteBatch.h:45) and by emissiveGroove (src/Graphics/Renderer.h:70). The verifier called the other two cited examples weak. The library dependencies themselves still hold.
- **Measurements** are of a "showcase level" that no longer exists. The dagger double-load bullet (:153) is stale, since m_modelCache now exists.

**Fix:** regenerate the module table, redraw the frame flow, then either amend Rule 3 or move that vocabulary to the Game side. Re-measure on crypt1 or eval_arena.

### C464. Comments still describe the deleted vit_exertion creep
*medium - cleanup - effort S*

SpendStamina's body says `vit_exertion` is gone and exertion now trains conditioning through GrantResourceXp (src/Game/DungeonWorld_Combat.cpp:480-487, Balance.h:188). Comments around it still say exertion feeds VIT:
- src/Game/DungeonWorld.h:476
- src/Game/DamageLedger.h:84 (the Growth reason)
- DungeonWorld_Combat.cpp:429, :442 and :570

DungeonWorld.h:3717 says GrantStatPoint is shared with SpendStamina, but its only caller is GrantSkillXp (Combat.cpp:105).

docs/skills.md still claims to describe "what the code does today", but it is out of date:
- It gives damage "x (1 + 0.08 x level)" and "+0.02 accuracy". These are now skill_damage and StanceAttack.
- It omits avoid, the armour skills, throwing and the three practices.
- It lists INT/WIL feeders as open.

**Fix:** reword the comments (one line each). Fold skills.md into combat.md and health-and-healing.md, or point it at them.

### C466. Game.h and Game.cpp comments contradict documented rules
*medium - cleanup - effort S*

Game.h is the class's handoff, and it states several decisions that were reversed:
- **Esc quits:** Game.h:113 says Esc outside play quits. Esc never quits.
- **Sheet pauses:** Game.h:30-32 says the sheet freezes the world, but Game.cpp:2159 says "NOT A PAUSE". The banner also lists only 6 of the 8 AppStates.
- **Assets:** Game.h:34 says assets load from next to the exe.
- **World switch relaunches:** Game.h:449-453, :820-826 and Game.cpp:90 say switching worlds relaunches. SwitchWorld now defers in-process (Game_Editor.cpp:203-206). `-newgame` is parsed nowhere, and RestartApp's `extraArgs` is never passed (Game_Wiring.cpp:186), so the parameter is dead.
- **Fixed roster:** Game.h:990 says the roster is never resized. ResetRoster replaces it.
- **Misplaced or orphaned comments:**
  - The harness comment at :865 belongs on the member at :993.
  - The CreateNewLevel comments at :350 and :440 are orphaned.
  - The banner at Game.cpp:505 sits above LoadWorld.
  - The bake comment at Game.cpp:694 belongs above StartBakeStep in Game_Editor.cpp:38.
  - Game.cpp:1478 describes ResetForEval, which lives in Game_Eval.cpp.

**Fix:** rewrite the banner from AppState, move or delete these comments, and drop `extraArgs`. CLAUDE.md's "Game.cpp is just the app state machine + wiring" is also untrue now.

### C473. CLAUDE.md says facing +1 is on-screen left; the code says right
*medium - cleanup - effort S*

CLAUDE.md:72, in the "memorize, they bite" list, says "Facing index +1 is on-screen LEFT (see Party.cpp comment)". The comment it cites says the opposite (src/Game/Party.cpp:281-283). Since the camera's X un-mirror (src/Graphics/Camera.h:66), +1 turns clockwise, to the on-screen RIGHT. The code agrees:
- StrafeRight and TurnRight use +1 (Party.cpp:288-298).
- CastSpell's left lane uses `faced + 3` (src/Game/DungeonWorld_Combat.cpp:2057).

A session trusting CLAUDE.md would write strafe or lane code inverted, which is the bug the note exists to prevent.

**Fix:** change CLAUDE.md to "+1 = clockwise = on-screen RIGHT" and keep the pointer to Party.cpp.

### C452. Stale projectile comments and docs
*low - cleanup - effort S*

src/Game/Projectiles.h:5-7 calls monster shots and thrown items future work. Both exist now; traps do not. The "cause not yet acted on" line (src/Game/DungeonWorld_Combat.cpp:2895) is true only of the proc burst. Cause is acted on at :2878 and in Throw.cpp:267. The `atk` comment at Projectiles.h:206 misses the Detonate and door-strike uses. docs/effects.md:480 sits in a dated section, so it needs an "as of" note, not a rewrite.

### C453. combat.md expects area attacks to kill a downed member; none can
*low - cleanup - effort S*

docs/combat.md:384 and CLAUDE.md say a hit on a member already at 0 kills them by overkill. In practice every party damage path skips downed members:
- ApplyBlastHit (src/Game/DungeonWorld_Combat.cpp:2765)
- monster projectile lanes (:2274)
- PickMeleeVictim (:1383)
- expiry procs (:2912)
- CollideParty

Only DoT ticks ever reach WoundMember's overkill branch. Decide once: drop these filters for area hits, or document that only DoTs finish the fallen.

### C457. CLAUDE.md's HUD, UIContext and Player-map paragraphs describe removed UI
*low - cleanup - effort S*

- CLAUDE.md:166 and :1125 say five UIContexts. src/Game/GameUI.h:731-737 declares seven.
- :1285 describes a fixed right-edge control panel. The docks are now floating panels (src/Game/ControlBar.h:4), and `log.hands_empty` is unused in all five .lang files.
- :1509 describes a Player-map key dock with its own flag. That dock was removed (src/Game/MapView.cpp:385).
- RosterMember lives in PartyHudTypes.h and HandSlot in HandSlot.h. There is no "Select tool".

### C459. DIAGRAMS.md describes the app before worlds loaded on demand
*low - cleanup - effort S*

These diagrams no longer match the code (docs/DIAGRAMS.md and its SVGs):
- level1.map is parsed at startup (:94).
- There is a `load.pillar` task (:129).
- RebuildGeometry is called (:142); it was removed.
- The character sheet pauses the world (:164).
- Lights update before the camera (:173). The code runs UpdateCamera first (src/Game/DungeonWorld.cpp:506).

The verifier found that nothing links to this doc, so the impact is low. Deleting it and its three SVGs is cheaper than redrawing them.

### C460. CLAUDE.md, combat.md and Combat.h say there are seven damage types; the catalog has eight
*low - cleanup - effort S*

Damage types come from `damagetypes.cat`, which has 8 entries: the seven listed plus `[starve]`. Three places still say seven:
- CLAUDE.md:329 ("Seven damage types")
- docs/combat.md:52 ("### Damage types (7)")
- src/Game/Combat.h:30 ("The live count is 7")

CLAUDE.md never names `damagetypes.cat`, even though its own SUPPLIES bullet uses `starve`. CLAUDE.md:336 also calls `natureResists` "the future race layer", but races already fill it (src/Game/Game_Party.cpp:81-87).

Fix: one line in the COMBAT bullet naming the catalog, the combat.md heading, and drop the count in Combat.h. The verifier kept this at low because Combat.h:12 already says the types are catalog data.

### C461. The damage-type catalog header says C++ names no types, but C++ names five
*low - cleanup - effort S*

The header written by src/Game/Project.cpp:43 (and repeated in the shipped `damagetypes.cat`) says "C++ names none of them". The code does name them:
- `bash` at src/Game/DungeonWorld.cpp:199
- `starve` at src/Game/Effect/SupplyEffect.cpp:20
- `earth`, `pierce` and `fire` at src/Game/Effect/DotEffect.cpp:22-35
- src/Game/Threat.cpp:37 picks physical types by name, not by the `physical` flag

A missing id only logs a warning (src/Game/Effect/Effect.cpp:31-33), so renaming `starve` through `typeset` would quietly make starvation deal type 0, which armour then reduces.

Fix: correct the header. Optionally add a required-ids check. None exists today, so that means building one.

### C462. CLAUDE.md names a floorfeatures.cat that does not exist
*low - cleanup - effort S*

CLAUDE.md:1931-1934 describes `floorfeatures.cat`. The real catalog is `surfacefeatures.cat`, and it covers both floor and ceiling features. The parser accepts `floorfeature` and `ceilingfeature` records (src/Game/DungeonMap.cpp:276), and the schema keys on `surfacefeatures` (src/Game/CatalogSchema.cpp:926). A reader would look for a missing file and never learn about ceiling features (vaults; `pit_ceiling` is a stairs.cat type, not one of these).

Fix CLAUDE.md, mention the `surface` field, and correct the same wrong name in the header comments of tools/BuildFloorCracked.py, BuildFloorDrain.py and BuildFloorGrate.py.

### C463. CLAUDE.md lists the wrong steps for AssetBaker's full bake
*low - cleanup - effort S*

CLAUDE.md:656-658 says a full `AssetBaker <assets>` run makes title art and party portraits. Neither is generated any more: portraits are bought and title_bg.png is committed art. The real sequence (tools/AssetBaker/Main.cpp:197-203) is textures, sounds, models, runes, mips, then model-image mips. tools/AssetBaker/TextureBaker.cpp:33 also mentions a "title baker"; the noise helpers are actually shared with ModelBaker and RuneBaker.

Fix the list and the comment. Softening the "nothing generated at runtime" heading is optional, since icons and liquid meshes are caches, not content.

### C465. DungeonWorld and Game comments describe old roles, removed features and missing functions
*low - cleanup - effort S*

The file banners are the handoff documents, and several are out of date:
- **Banners:**
  - src/Game/DungeonWorld.h:2-8 leaves out the editor backend, the multi-level store, undo, census and validation.
  - src/Game/Game.h:30-31 says the character sheet freezes the world, but src/Game/Game.cpp:2159-2166 keeps updating it.
  - Game.h:34 says assets load from beside the exe.
  - src/Game/DungeonWorld_Validate.cpp:5 says the file "only gathers", but it also holds the InstallLevel functions and the stair helpers.
- **Removed features:** `vit_exertion` is still described at DungeonWorld.h:476, DungeonWorld.cpp:94 and DamageLedger.h:84. Also stale: `MagicSystem::Update` (h:911), the "torch palette" (h:1365), "monster effects not saved" (h:2425), "buttons have no model" (h:2711), and mana regen "scaled by intelligence" (cpp:947).
- **Functions that do not exist:** `UpdateMonsterIcons` (h:2347) and `BakeItemIconsIfNeeded` (h:4169).
- **Comments with no code, or on the wrong code:**
  - src/Game/DungeonWorld.cpp:2350-2354 has two comment blocks with no function under them. One calls a direct health write "the one place a member takes damage", which contradicts the fx::Deal rule.
  - In DungeonWorld.h, about a dozen doc blocks sit above the wrong declaration (the Door doc on Breakable, the Decoration doc on PropTextures, others).
  - The ConsumeAIPlans comment sits above TickLockstepAI (cpp:2178).

Fix: one sweep. The verifier noted that "simulation of one level" (h:1532) is defensible, because only the active level simulates.

### C467. File splits left comments swapped between functions, plus wrong notes and unused includes
*low - cleanup - effort S*

- **Swapped:** StartBakeStep's doc sits above `RunLoadTasks` (src/Game/Game.cpp:694-696), and RunLoadTasks' own doc is orphaned at the end of src/Game/Game_Editor.cpp:1283-1284. OpenTypeEditor's doc sits above CreateAuthoredType (Game_Editor.cpp:577).
- **Wrong:**
  - FinishBake's note (Game_Editor.cpp:487-489) says writes go to an asset copy beside the exe; there is only one asset tree now.
  - The StartRestyleBake log line still says "wall style" (Game_Editor.cpp:1222).
  - Game_Generate.cpp:63 says there is no shared DirName, but `DirToken` exists (Entity.h:41).
- **Stale stair header:** the header at src/Game/DungeonWorld_Remote.cpp:22-30 describes editing the .map text directly; the code edits in-memory stashes.
- **Wrong premise:** Remote.cpp:145-148 and DungeonWorld_Resize.cpp:64-66 say a flat_map insertion invalidates a `DungeonMap&`. The map holds `unique_ptr`s, so the reference stays valid.
- **Unused includes:** `<cstdlib>` "for atof" in the Editing, Remote, Undo and LevelIO files. Remote and Undo also include File.h, Paths.h, `<filesystem>` and `<format>` without using them.

Fix: move each comment back to its function, correct the wrong ones, and trim the include blocks.

### C468. Comments in Core still describe behaviour from earlier build phases
*low - cleanup - effort S*

These are public-header contracts that no longer match the code:
- src/Core/ThreadManager.h:144-148 says Restart blocks on a wedged worker. It now force-terminates through StopOrTerminate.
- ThreadManager.h:91-93 says a WorkerId is an index that is never invalidated. Reap removes stopped workers, and the id is not an index.
- src/Core/CrashHandler.h:19-24 says the handler does not symbolize. FaultFilter now walks and logs the stack.
- src/Core/Profile.cpp:60 says about 2 MB; the real figure is about 20 MB.
- src/Core/Profile.h:453 names `Collector::Publish`; the function is `CopyAndReset`.
- src/Core/StackTrace.h:37 says Describe calls Init lazily; nothing calls Init.

Remove the step and phase notes too (ThreadManager.h:19, Diagnostics.h:83).

### C469. PerfMonitor's header still describes the design from before the sampler thread
*low - cleanup - effort S*

- src/Platform/PerfMonitor.h:66-67 says Tick runs the throttled OS queries. Tick now only reads atomics; a worker does the queries.
- PerfMonitor.h:90 names `StartGpuWorker`, which does not exist. The function is StartOsSampler, though the member is still called `m_gpuWorker`.
- `GetProcessMemoryInfo` is called by two copies of the same code (PerfMonitor.cpp:29-33 and 118-120).

Fix the comments, rename the member, and have Sample call QueryProcessMemory.

### C470. Combat comments describe the old hit roll and promise checks that do not exist
*low - cleanup - effort S*

- src/Game/Combat.h:253 describes a clamped accuracy-minus-evasion roll. ResolveAttack is an opposed d100 roll (Combat.cpp:47-52). src/Game/Effect/Effect.h:183 repeats the old wording.
- Combat.h:18-21 says IsPhysical has one caller. It has three, including DungeonWorld_Combat.cpp:1003.
- src/Game/Roll.h:40 says every roll rule is authored, but `sides` and `openEndedDefense` are never set.
- src/Game/Mishap.cpp:15-17 promises a compile error for a Kind with no token. None happens: a new Kind gets "?".
- SpendExertion's doc (DungeonWorld_Combat.cpp:496-511) sits about 250 lines above the function.
- TickAutoAttack repeats one paragraph twice.

For Mishap, add a `Count` enumerator and a `static_assert` so the check it promises exists.

### C471. Loading comments say a missing texture aborts the game; it gets a placeholder
*low - cleanup - effort S*

src/Game/AssetUtil.h:57 and its banner say a missing required texture aborts. LoadTextureFile actually returns a magenta checker (AssetUtil.cpp:186-193). LoadPbrSet's comments say "dies" (DungeonWorld_Load.cpp:367, 379). A missing `_n` normal map also gets that magenta checker as its normal (Load.cpp:388); a flat (128,128,255) placeholder would be the better fallback.

Also:
- "Wall Style rebake" is still cited at Load.cpp:2302, DungeonWorld.h:100 and Game.h:1244.
- Three doc blocks sit above the wrong function (Load.cpp:64, 1809, 2152).
- DungeonWorld_LevelIO.cpp:14 has an unused atof include.

### C472. DungeonMap's "static layer" banner is wrong, and other data-model comments are stale
*low - cleanup - effort S*

src/Game/DungeonMap.h:2 says the map holds only what never changes during play. In fact WallSconce carries the saved `flipped`, `empty`, `torch` and `torchCharge`, and WallNiche carries `open`. StashStaticMap (DungeonWorld_LevelIO.cpp:118) and the new-game reset (DungeonWorld_Save.cpp:99-102) clear these. The verifier found no live bug, only the stale banner.

Other stale comments:
- DungeonMap.h:10: the record grammar list leaves out floor and ceiling features.
- DungeonMap.h:302: "revision 0 at load" is wrong.
- DungeonMeshBuilder.h:4: says "runs once at load" and "position hash".
- CatalogSchema.cpp:103: still mentions pillars.
- Project.h:160: lists 15 of the 28 catalog keys.
- Project.h:18 and Serialize.h:10: their examples put `;` comments after values, and the parser keeps that text as part of the value.

Against a documented decision: the finder proposed moving the fire and niche flags into a dynamic overlay. CLAUDE.md (FIRE AND LIGHT) chose flips on the map, so that change is optional. The concrete fix is the banner.

### C474. Character, party and settings comments point at removed functions and old rules
*low - cleanup - effort S*

- src/Game/Character.h names functions that do not exist: Evasion (:6) and ManaRegenPerSec (:107).
- It also gives the old resource formula (:79) and says monsters "will" carry effects (:49); they already do.
- It says the race system is still to come (:351), has an orphaned dodge comment (:347), and repeats :87-91 almost word for word at :96-99.
- src/Game/Character.cpp:178 says Brand has STR 10; he has 16.
- src/Game/Balance.h:73 describes `roll_scale`, which was removed.
- Party.h:28 and GameSettings.h:87 say the key binds are on the Game tab; they are on Controls.
- Party.h:230 names `kLookReturnTime`, which does not exist.
- GameSettings.h:184 sits above the wrong fields.

Fix or delete each comment in place.

### C475. Stale dev-command comments and an incomplete `arena` synopsis

*low - cleanup - effort S*

Several comments in `src/Game/Game_DevCommands.cpp` are out of date or sit on the wrong code:
- A staged-loading banner sits above PrintPalette (:1675).
- In the editor lambda, three verb comments are on the wrong verbs: the resize comment is on `view` (:267), the `rev` comment is on `tool` (:313), and a place/erase/move comment is on `disarm` (:252).
- `src/Game/Game_DevDungeons.cpp:4` says the file is "past three thousand lines". It has 1821.
- `src/Game/DevCommandArgs.h:7` lists the wrong files that include it.
- A few includes are unused.

Separately, the `arena` params and refusal text leave out `room` (`src/Game/Game_DevEval.cpp:139`, :146), which the expedition and arena suites use. Its output (:162) never prints the party's cell, which for `room` differs from the centre. The fix is to move each comment onto its verb, add `room`, print `party sx,sz`, and drop the unused includes.

### C476. Dev console comments describe a layout that has changed

*low - cleanup - effort S*

- `src/Game/DevConsole.h:11` omits the console's own `profile` command, and its list of Game commands is badly out of date (there are about 130).
- `DevConsole.h:649` points to a note in the .cpp that does not exist.
- `src/Game/DevConsole_Panel.h:4` says six files include it. Seven do.
- `DevConsole.h:164` says three readout sections. There are four, and Health is the one whose state is not saved.
- `src/Game/DevConsole_Profile.cpp:506` reserves `line * 2.4f` for a header that is now one line, so a blank line shows.

Fix the comments and trim that reservation.

### C477. Orphaned and stale HUD comments, and one skill shown in two colours

*low - cleanup - effort S*

- Each of `src/Game/HandSlot.h:101` and `src/Game/SpellbookPanel.h:168` ends with a block describing another class. The second describes InventoryWindow, which no longer exists.
- `SpellbookPanel.h:145` and `src/Game/PartyHudDraw.h:96` name DrawRuneFace. The draw goes through DrawRuneGlow.
- `src/Game/PartyHud.h:4` and `src/Game/SlotGrid.h:5` (which says "2.4 m") are also wrong.
- One real mismatch: the status line colours a skill name with SkillRow::tint, but its bar uses SkillBarColor (`src/Game/CharacterSheet.h:457`). Weapon, defence and reserve skills therefore show two different colours.

Fix: use SkillBarColor for the name and drop `tint`. Also fix or delete the comments, and remove the claim that kHealthRed and the other reserve colours match bar.hlsl. They are close but not equal.

### C478. Map view and editor comments describe removed tools and a player-map key that is gone

*low - cleanup - effort S*

- `src/Game/MapView.h:8` and :337 describe a right-docked key in Player mode with its own flag. The key is editor-only now (`MapView.cpp:385`). CLAUDE.md's Player-mode bullet still names `map_player_key_collapsed`, and no source file uses it.
- `MapView.cpp:705` says onNewLevel returns a stem, which `MapView.h:96` contradicts.
- `MapView.cpp:816`, `src/Game/MapEditor.h:14`, h:169 and h:240 still name Select/Erase tools that no longer exist.
- `src/Game/MapView_Tools.cpp:6` lists five tools. The strip has nine.
- Other stale lines:
  - `MapEditor.h:166` and :367 say only Monsters are configurable. Every category is now.
  - The `"" = not creatable` note is wrong, because every category has a catalog key.
  - The three Creatable paragraphs at h:584 contradict each other.
  - The comment on `m_settings` is wrong.
  - In `MapColors.h:40` the issue-box comment sits above kPowerBand instead of kIssueError.

Fix the comments and update CLAUDE.md. Two small tidies come with it: give the duplicated swatch colour a name, and size `m_icoCats` (16 slots for 12 icon names) from CategoryIconNames.

### C479. Stale or false comments in the inspectors and dialogs

*low - cleanup - effort S*

- `src/Game/InstanceInspector.h:7` mentions a Save/Close footer. Close is now the corner box.
- `InstanceInspector.cpp:61` says six inspectors. There are seven.
- `src/Game/NicheInspector.h:53` uses `kFacingH`, a constant that was removed.
- `src/Game/LevelSettingsDialog.cpp:270` names StemTitle. The class is EditableTitle.
- `src/Game/ValidateDialog.cpp:78` says the rows are padded into alignment. They are an unpadded `std::format` in a centred button.
- ButtonInspector says its list holds door names only. It also lists niche names (`Game_Inspect.cpp:183`).
- ProjectileInspector gives a reason for not deriving from InstanceInspector that no longer holds.
- `DialogLayout.cpp:134` claims the help overlay does no allocating after its first frame, but it rebuilds a string every frame. This is not a guarded frame, so it breaks no rule.
- `Game.cpp:2379` says "seven" and then "six".

Correct each comment. For ValidateDialog, either left-align the rows or drop the claim.

### C480. CLAUDE.md is wrong about MSVC's debug vector move constructor

*low - cleanup - effort S*

CLAUDE.md:113 says iterator debugging makes vector's move constructor not noexcept, so growth copies elements. Both installed toolsets declare it `noexcept` (`include/vector:761` in 14.51, :759 in 14.44). The proxy allocation happens inside that noexcept body, so a vector of vectors moves on growth.

The same passage says "each copied anim channel re-allocates both its buffers". That is stale too: `AnimationChannelData` (`src/Assets/Model.h:105`) now holds ranges into pooled clip data and owns no buffers.

Fix: rewrite the note to say the debug move allocates a proxy but is still noexcept, and credit the debug-vs-release gap to that proxy. Drop the copy and channel claims, but keep the advice to reserve the clip vectors.

### C481. The check-* skill files describe old harnesses

*low - cleanup - effort S*

The following files under `.claude/commands/` are out of date:
- `check-ingame.md` says four screens and no sheet sweep. InGameTest sweeps 32 screens, including `sweep_sheet`.
- `check-health.md` describes three retries. `HealthTest.ps1:236` says "NOT retried".
- `check-eval.md` lists six suites. `Eval.ps1` has 13.
- `check.md` gives the wrong time and tier contents, and its family table leaves out check-eval and check-profile.
- `check-selftest.md` omits build-profile.
- `check-full.md` gives an incomplete list of what it adds.

Fix: regenerate the lists from `CheckAll.ps1 -List` and `Eval.ps1 -List`.

The verifier noted that two smaller points are loose rather than outright wrong: "the diagnostics harness" in check-full may mean HealthTest, and the "never reached the log" wording.

### C482. Script usage lines teach the `-File` comma-list trap

*low - cleanup - effort S*

The usage lines in `tools/FetchTextures.ps1:26`, `tools/FetchModels.ps1:27` and `tools/SortTextureDownloads.ps1:26` all pass comma lists through `powershell -File`. That passes `a,b,c` as one string, which matches nothing, so the run reports "Nothing imported". `SortTextureDownloads.ps1:237` itself warns about exactly this, and so does CLAUDE.md.

The FetchModels example also names `dagger,kukri`, but the table has `khukri` (:169) and only a `viking_dagger`, no plain `dagger`.

Fix: rewrite the examples in the `-Command` form (or use single names) and correct the example model names.

