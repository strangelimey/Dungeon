# Combat depth

The combat-depth thread (branch `combat-depth`, queued after the magic-system
thread). Scope settled with Michael 2026-07-07: everything below is IN —
weapon stats + per-verb profiles, equipment armor + hit severity, stamina
costs, death/revive, monster swing animation + poison/bleed, and reach
(movement.md Phase 6) as the final phase.

Combat.h stays the one strike resolver: profiles in, result out, no state.
Every phase here just builds richer profiles (or richer consequences) around
that seam.

## Current state (what each phase starts from)

- **Weapons are flavour.** `PartyAttack` (DungeonWorld.cpp) builds its
  AttackProfile purely from `Character::AttackDamage()` (4 + 0.5×STR) ×
  the weapon-class skill scale (×(1+0.08L), +0.02L accuracy). The held
  item contributes only its `skill` id. `Character::AttackInterval(hand)`
  is an explicit stub: dexterity-only, ignores the hand.
- **Verbs are flavour.** GameUI's `kMeleeUses` (punch/kick/stab/slash/
  chop/bash/swing/melee) all collapse into `onHandAttack(member, hand)` —
  the verb is dropped before the world sees it.
- **Armor is spell-only.** `Character::Armor()` reads the earth ward's
  magnitude; worn equipment (leather_armor exists, `armor` category)
  contributes nothing. Hit severity (`WoundMember`) is raw damage
  thresholds (<5/<10) marked placeholder.
- **Stamina is decorative.** Nothing spends it (food restores it); no regen.
  Mana regenerates (`ManaRegenPerSec`), stamina doesn't.
- **Down is a dead end.** health 0 = `!IsAlive()`: the member stops acting,
  monsters skip them, party wipe latches when all four fall. No revive, no
  unconscious-vs-dead distinction. No healing source exists yet (food is
  stamina-only; no heal spell, no potions).
- **Monster swings are invisible.** `MonsterAttack` already sets
  `monster.attackReq`; the state machine yields Attack — but only
  `skel_human` has authored `anim_attack` clips (the anim-library import).
  The procedural rigs (ModelBaker) have no attack clip, so the request
  plays nothing.
- **No status effects beyond wards.** `StatusKind` has one value (Ward),
  but the whole pipeline — HUD name-band icons, sheet Effects tab, save
  "effect" lines with unknown-token skip — is kind-generic.

## Design

### Phase 1 — weapon damage & speed (items.cat)

New weapon fields, numeric like the spell overrides:

    damage = 6      ; base damage of a clean hit with this weapon
    speed  = 1.2    ; seconds between swings (before dexterity)

- Armed damage: `weapon.damage + 0.25×STR`, then the skill scale as today.
  Unarmed keeps `AttackDamage()` (4 + 0.5×STR). Strength still matters
  armed, but the weapon is the base — upgrading it is felt.
- Interval: `weapon.speed × (1.15 − 0.015×DEX)`, clamped [0.6, 2.0]s.
  Unarmed keeps the current dex-only formula. Character.h can't see
  ItemKind (layering), so `AttackInterval(hand)` grows a weaponSpeed
  parameter (0 = unarmed) and `PartyAttack` feeds it from `ItemKindFor`.
- Author the four daggers (fast, low damage — speed ~0.9–1.1, damage
  ~5–7) and a new `[club]` — `skill = blunt`, slow and hard (speed ~1.6,
  damage ~10), `command = bash, swing` — so the second weapon class
  finally trains. No model yet → tablet placeholder (a fab.com mace is a
  candidate buy, Michael's call — see docs/costs.md).
- Missing fields default to the unarmed-equivalent (damage 0 → attribute
  base, speed 0 → dex formula), so non-weapon holdables swing unchanged.

### Phase 2 — per-verb profiles

Thread the verb through: `ExecuteUse` passes `cmd` → `onHandAttack(member,
hand, verb)` → `Game` → `PartyAttack(member, hand, verb)`. Profiles are a
typed C++ table (content-stays-data-driven rule: typed fields + C++
behaviour, not a scripting hook):

| verb  | damage | accuracy | interval | note                        |
|-------|--------|----------|----------|-----------------------------|
| stab  | ×0.8   | +0.05    | ×0.8     | fast, precise               |
| slash | ×1.0   | +0.00    | ×1.0     | the baseline                |
| chop  | ×1.3   | −0.05    | ×1.25    | committed, heavy            |
| bash  | ×1.15  | −0.05    | ×1.2     | later: stun hook (Phase 6+) |
| punch/kick/swing/melee | ×1.0 | +0.00 | ×1.0 | neutral            |

Numbers are feel-test fodder. A weapon's verb list (its `command` field)
already curates which profiles it can use — a stab-less khukri stays
stab-less.

### Phase 3 — equipment armor + hit severity

- items.cat `armor = <flat soak>` on armor/clothing entries
  (leather_armor ~3, tunic ~0.5). `Character::Armor()` sums WORN slots
  (equipment[] minus the two hands) + the earth ward as today. Needs the
  same layering treatment as speed: the world sums catalog values and the
  sheet shows the total (a derived-stats line on the sheet is in scope).
- Hit severity goes relative: fraction of the TARGET's maxHealth —
  <10% small, <25% medium, else hard — replacing the raw <5/<10. "What a
  hit means" scales with the victim, so a late-game 8-damage tap stops
  reading as "hard".

### Phase 4 — stamina costs + exhaustion

- A swing spends stamina: `1 + 0.4×weapon.weight`, verb-scaled (chop/bash
  ×1.5). Casting stays mana's business.
- Regen: `0.5 + 0.02×maxStamina` per second while not swinging (any swing
  resets a short 1.5s regen holdoff), in the same world tick that regens
  mana. Food keeps its instant restore.
- Exhausted (stamina hits 0): swings still land — blocking them entirely
  feels terrible in a real-time crawler — but at ×0.5 damage and ×1.5
  interval until stamina climbs back over a 10% recovery threshold
  (hysteresis so it doesn't flicker at the boundary). Log line + the bar
  itself communicate it.

### Phase 5 — death & revive

0 HP splits into UNCONSCIOUS vs DEAD:

- A member dropping to 0 is **unconscious** (today's down state: skipped
  by monsters, acts on nothing). They **stabilize on their own**: after
  30s with no monster within aggro of the party, they wake at 20% health.
  The wake timer is live-transient (like hand cooldowns) — a save/load
  restarts it, no new save lines.
- **Dead** needs deliberate overkill: a hit that lands on a member ALREADY
  at 0 (possible once monsters get area/special attacks) or a single blow
  ≥ 150% of maxHealth kills outright. Dead members don't wake; a future
  resurrection mechanic (altar/spell) is the revive path — out of scope
  here beyond the flag. `bool dead` on Character, one save line
  (v17 "dead" per slot; absent = alive-or-unconscious as today).
- Party wipe stays as-is (all members at 0, unconscious or dead) — but
  wake-on-stabilize means a wipe now requires the monsters to actually
  finish the job before wandering off; review the latch at feel-test.
- Healing sources (potions, a water heal spell) remain the magic queue's
  business; stabilize keeps death survivable until they land.

### Phase 6 — monster swing animation + poison/bleed

- **Swing anim:** the request plumbing exists (`attackReq` →
  DesiredState). Work: (a) verify skel_human's clips fire in-game; (b)
  bake a simple procedural attack clip (a ~0.4s forward lunge/swipe) into
  ModelBaker for the procedural rigs (skeleton family, blob squash-lunge,
  mummy) so every monster telegraphs its hit.
- **Poison/bleed:** `StatusKind::Poison` and `::Bleed` (save tokens
  "poison"/"bleed" — old saves skip unknown kinds, so no version bump for
  the effects themselves). monsters.cat opt-in per type:
  `poison = <dps> <seconds> [chance]` (ditto `bleed`) applied on a LANDED
  MonsterAttack blow; same-kind reapply refreshes (matches ward recast
  rule). Tick site: the world tick that ages effects also applies
  `magnitude × dt` through `WoundMember` (severity floor, no splat spam —
  a DoT wound skips the flash unless it downs). Tint: poison=earth green,
  water=red? — no, bleed rides fire red, poison earth green via the
  existing school-tint convention. Lang: `effect.poison`/`.desc`,
  `effect.bleed`/`.desc`, `log.poisoned`/`log.bleeding` ×5 languages.
  Candidate carriers: blob poisons, skel_lurker bleeds.

### Phase 7 — reach (movement.md Phase 6, unblocked by Phase 1)

- items.cat `reach = melee|polearm` (default melee). Party rank: roster
  slots 0–1 are FRONT, 2–3 REAR (the classic DM convention; a formation
  editor is out of scope). Melee weapons swing from the front rank only —
  a rear member's swing logs "can't reach" (new log key); polearms swing
  from either rank. Spells/ranged already ignore rank.
- Monster side per movement.md: `reach` catalog stat lets a rear-slot
  (queued) monster strike the party past its front rank — the atPost
  gate widens for reach attackers. Symmetric with the party rule.
- This is the phase most likely to need feel-testing and the movement
  thread's slot/formation machinery — hence last.

## Phase order & save ladder

1 → 2 → 3 → 4 → 5 → 6 → 7, committed per phase. 1–2 are one seam (weapon →
profile); 3–5 are independent of each other; 6 is world/content-side; 7
depends on 1. Save: only Phase 5's "dead" line needs v17; effects and
catalog fields degrade gracefully by design.

## Conventions to keep (from the closed threads)

- CMakeLists lists Game sources BY HAND — any new .cpp must be added.
- New user-facing strings: en.lang first, then the other four.
- Feel-test with Michael on the running instance mid-session; timed
  effects expire across tool-call gaps — cast + screenshot in ONE script.
- Save/load: new dynamic state rounds through CaptureState/ApplyState +
  SaveData (docs/skills.md and the v13–v16 lines as the pattern).
