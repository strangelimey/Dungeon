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

## The attack formula (Michael, 2026-07-08)

The scaling/balance spine the phases hang off. Core rule: **an ATTACK
(verb) is the atom, and each attack deals ONE damage type, globally** —
stab is always pierce, whoever swings it. A weapon type is just its list
of attacks (already the items.cat `command` field), so there is nothing
per-weapon-per-attack to balance: tune an attack once and every weapon
carrying it follows. Built up in parts; part 1 (types + tables) settled
below, the next parts extend this section.

### Damage types (7)

| Physical | Elemental |
|---|---|
| slash, pierce, bash | fire, earth, air, water |

### Attacks (global; one damage type each)

The "character" column becomes the attack's formula numbers in the next
part; the Phase 2 verb table below is those numbers' first cut and gets
ABSORBED into this table when the formula lands.

| Attack | Damage type | Character |
|---|---|---|
| stab   | pierce | fast, precise, light |
| jab    | pierce | quickest poke (spear/staff tip) |
| thrust | pierce | committed spear-drive, heavy pierce |
| slash  | slash  | the cutting baseline |
| hack   | slash  | a harder slash — slower, heavier |
| chop   | slash  | heaviest cut, least accurate |
| bash   | bash   | blunt baseline |
| swing  | bash   | broad arc, a club's bread-and-butter |
| punch  | bash   | unarmed light |
| kick   | bash   | unarmed heavy |

### Weapon types → attacks & associated stats

Current items first, then the natural next weapons (the table doubles as
the sourcing roadmap — models to buy/import). The stats column is formula
part 2 (see "Associated stats" below); Michael set dagger/club/sword,
the *(proposed)* rows await his red pen.

| Weapon | Skill class | Attacks (→ type) | Assoc. stats |
|---|---|---|---|
| Dagger, snake dagger | blade | stab → pierce, slash → slash | DEX |
| French dagger | blade | stab, slash | DEX |
| Khukri | blade | chop → slash, slash | STR+DEX *(proposed — it's a chopper)* |
| Club | blunt | bash, swing → bash | STR |
| Bare hands | unarmed | punch, kick → bash | STR *(proposed, matches old creep)* |
| *Sword* (future) | blade | slash, stab, hack | STR+DEX |
| *Axe* (future) | axe (own class — too dissimilar to a sword) | chop, hack, bash (haft) | STR *(proposed)* |
| *Mace* (future) | blunt | bash, swing | STR *(proposed)* |
| *Warhammer/maul* (future) | blunt | bash, swing (a harder bash rides the weapon's numbers) | STR *(proposed)* |
| *Spear* (future) | new polearm class — ties into reach (Phase 7) | thrust, jab, bash | STR+DEX *(proposed)* |
| *Staff* (future) | blunt | swing, jab | DEX *(proposed)* |

### Spells & everything else

One rule instead of a table: a damaging spell's type is its SCHOOL —
Flame/Fireburst → fire, Waterbolt/Splash → water, Rock/Slingshot →
earth, Gust/Push → air. Future enchanted weapons and monster specials
pick a type the same way; monster melee gets a per-type damage type when
the defender side (the next part) needs it.

School associated stats (part 2): **earth + fire → INT, air + water →
WIL** (willpower — settled: no wisdom rename, the roster's stat is used
as-is; it already feeds the cast fumble roll, Magic.cpp).

### Associated stats (part 2 — Michael, 2026-07-08)

Every attack SOURCE — a weapon type, a spell school — has one or more
associated stats. Their AVERAGE is the stat input to the attack:

    statInput = (stat1 + stat2 + ...) / count     e.g. (STR + DEX) / 2

- The average drives the **attack bonus** — it replaces the ad-hoc
  strength/dex terms in today's damage formulas (weapon `+0.25×STR`,
  unarmed `+0.5×STR`); the exact bonus coefficient is a later part /
  feel-test knob.
- The same stats are what **train on use**: a landed blow / successful
  cast creeps the source's associated stats, REPLACING the per-skill
  creep table in docs/skills.md (fire→STR, air→DEX, earth→max-stamina,
  water→max-health, blade→DEX, blunt/unarmed→STR) when it lands. Skills
  themselves still train by use exactly as before — only the creep
  TARGET changes. Multi-stat sources SPLIT the creep gain evenly across
  their stats (settled — a sword doesn't train stats twice as fast as a
  club).
- Earth/water's creep stops raising max stamina/health — resource
  growth moves to the resource formula (part 3 below).
- Where it lives: items.cat `stats = str, dex` per weapon; the school →
  stat pairs are a fixed typed table (they're four lines).
- Settled: no wisdom rename — air/water use WILLPOWER as-is.

### The resource formula (part 3 — Michael, 2026-07-08)

Resource maxima are DERIVED from stats, the same base-plus-stat-average
shape as the attack bonus (each `k` a balance knob; `base` is the
authored per-member value, so class identity survives — the mage keeps
the big mana base):

    maxHealth  = base + k_h × VIT
    maxStamina = base + k_s × (STR + VIT) / 2
    maxMana    = base + k_m × (INT + WIL) / 2

Resources grow because stats grow. This also fixes maxMana never growing
at all today, and finally gives vitality a job.

**VIT trains from exertion, and STAMINA IS THE EXERTION METER**: every
point of stamina SPENT feeds VIT's creep pool — no separate tracking of
swings-per-minute or distance-run. Use drains it in the moment, builds
it over time (a conditioning curve). Consequences:

- Phase 4 (stamina costs) becomes this part's engine: swings cost
  stamina as planned, and MOVEMENT joins them — a small per-step cost so
  sustained marching genuinely exerts, while the regen holdoff keeps
  strolling-with-pauses net-neutral. Exhaustion stays penalties-not-
  paralysis (it must never wall the player).
- Taking damage is NOT a VIT source — pure exertion training (revisit
  later if being wounded should toughen too).
- Mana spending already trains INT/WIL via the school stats (part 2), so
  the mage's mana pool grows by casting; the fighter's stamina/health by
  fighting and marching. Symmetric, no extra pools.
- Save: v17 stores the authored `base` per resource per member (old
  saves back-solve base from their grown maxima); current health/
  stamina/mana keep their stored values, clamped to the derived max.

### The defender side (part 4 — Michael, 2026-07-08)

A defender's response is three gates, in order:

1. **Evasion** — does it hit at all? Type-agnostic; the existing
   accuracy − evasion roll, clamped [0.05, 0.95]. Untouched. Party
   evasion derives from DEX (the agility stat — it also feeds accuracy
   and swing pace); MONSTER evasion stays a flat authored monsters.cat
   value (settled: monsters keep authored numbers, no stat blocks, for
   now — revisit if buffs/debuffs ever need to target a monster's DEX).
2. **Soak + resistance** — a SMALL flat soak, then the per-type
   multiplier:

       final = (rolled − soak) × (1 − resist[type]),  floor 1 on a hit

   Every defender carries one seven-cell RESIST table (the damage
   types). Positive = shrugged off, NEGATIVE = vulnerability — the
   payoff of damage types (the skeleton laughs at your stab; bring the
   club). Summed resists clamp to ±0.8; only an authored NATURE cell of
   1.0 (a fire elemental vs fire) reaches immunity.
3. **Floor** — a landed blow always stings (the existing ≥1 rule).

Resist/soak sources just sum into the same cells:

- **Nature** — monsters.cat per type (`resists = pierce 0.5, slash 0.25,
  bash -0.5` on the skeleton; the mummy takes `fire -1.0`; absent = 0)
  plus the existing flat `armor` as the monster's soak. THE PARTY GETS A
  NATURE LAYER TOO — race resists (a minotaur is a lot tougher than a
  ratling): authored per member now, the proper race system arrives with
  party creation; the summing treats it as just another source.
- **Equipment** — armor pieces author their resist cells (leather ~
  `slash 0.2, pierce 0.1, bash 0.1`) plus a small flat `armor` soak;
  worn pieces sum. This IS Phase 3's equipment armor.
- **Wards** — Stone Skin's magnitude converts from flat armor to
  physical resist (earth = the school that hardens, in formula terms).
  Fire's burn-back, water's soak pool, air's deflection keep their
  special behaviours — reactions, not resistances.

### The full formula, first cut (part 5 — Michael, 2026-07-09)

The assembled model. Every named constant is a KNOB — none lives in
code (see "Where the knobs live" below). First-cut values are mostly
today's numbers, renamed.

Damage — ONE shape for weapons, fists, and spells:

    rolled = (base + stat_damage × statAvg)
             × attack.dmg × (1 + skill_damage × skillLevel) × jitter
    final  = (rolled − soak) × (1 − resist[type]),  floor wound_floor

`base` = the weapon's catalog damage / the `unarmed_base` knob / a
spell's mana-derived power; `statAvg` = the source's associated-stat
average (part 2); `attack.dmg` = the per-attack multiplier (the Phase 2
verb numbers, now catalog data).

To-hit — **accuracy is always DEX** (settled: aiming is agility, no
matter what powers the blow; damage uses statAvg, to-hit does not):

    chance = clamp(acc_base + acc_stat × DEX + acc_skill × skillLevel
                   + attack.acc − evasion,  hit_floor, hit_ceil)

The knob sheet (first cut):

| Knob | Value | Does |
|---|---|---|
| unarmed_base | 4 | fist "weapon damage" |
| stat_damage | 0.25 | damage per point of statAvg |
| skill_damage | 0.08 | damage multiplier per skill level |
| damage_jitter | 0.15 | ± roll on every hit |
| acc_base / acc_stat / acc_skill | 0.55 / 0.02 / 0.02 | the to-hit line (stat = DEX) |
| hit_floor / hit_ceil | 0.05 / 0.95 | nothing's ever sure |
| resist_clamp | 0.8 | max summed resist (part 4) |
| wound_floor | 1 | a landed blow stings |
| speed_stat / interval_min / interval_max | 0.015 / 0.6 / 2.0 | DEX shaves swing pace, clamped |
| spell_stat | 0.01 | % spell power per statAvg point |
| creep_rate | 0.04 | stat creep per skill-XP (today's kStatCreepPerXp) |
| vit_exertion | 0.02 | VIT creep per stamina point spent (part 3) |
| k_health / k_stamina / k_mana | 1.0 | resource points per stat point (part 3 maxima) |

**Where the knobs live (Michael's requirement: editor-tweakable for the
whole dungeon):** a `balance.cat` in the PROJECT catalog folder —
per-dungeon scope, riding the same save/`synctosource` path as every
catalog — loaded into one typed tuning struct every formula reads
through. Editor: a Balance dialog (monster-config-dialog pattern), rows
generated from a fields table like GameSettings' kThemeFields (a new
knob = one table row, no new UI code). Two tabs: Formula (the sheet
above) and Attacks (the per-attack dmg/acc/pace numbers from
attacks.cat). Values apply live; Save writes the .cat. With attacks,
weapon stats, resists, and this sheet all in project catalogs, the
ENTIRE combat model is editor-authorable — a total conversion
rebalances without touching code.

### Settled calls (Michael, 2026-07-08)

- **Axe is its own skill class** — too dissimilar to a sword. Lands with
  the first axe; docs/skills.md gets the class (and its stat-creep row)
  then.
- **No bash on the daggers yet** — the blades go pure-edge (khukri:
  chop/slash, french dagger: stab/slash). "Yet": pommel strikes can
  return once per-type resistances make them tactically interesting.
- **No crush attack** — a maul's harder bash is just bash with a heavy
  weapon's numbers; the attack table stays lean.

### Implementation (LANDED 2026-07-09, commits da9bb8f + 55d1ca0)

The whole formula is built: Game/Balance.h/.cpp owns the knob struct
(kBalanceFields drives load/save/dialog), the attack identity table
(attacks.cat overrides the numbers — the spells.cat pattern), the
school helpers, and the stats=/resists= parsers. Combat.h carries the
seven DamageTypes, ResistTable, typed profiles, and StrikeRules.
PartyAttack assembles the attack side; PartyDefense/MonsterDefense the
defender side; GrantSkillXp creeps the source's stats; maxima derive
through Character::RecomputeMaxima (save v17 stores the bases,
pre-v17 back-solves). The editor map's Balance header button opens
BalanceDialog (Formula + Attacks tabs, live apply, Save → the project
catalogs) — the entire model is editor-tweakable per dungeon.
SpendStamina is the Phase 4 exertion hook, plumbed but unwired.

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

SUPERSEDED in shape by the attack formula part 4 (the defender side):
armor pieces author per-type resist cells PLUS a small flat `armor`
soak, and the party's defense sums nature (race) + equipment + wards
into one resist table. The sheet shows the summed defense (a
derived-stats line is in scope).
- Hit severity goes relative: fraction of the TARGET's maxHealth —
  <10% small, <25% medium, else hard — replacing the raw <5/<10. "What a
  hit means" scales with the victim, so a late-game 8-damage tap stops
  reading as "hard".

### Phase 4 — stamina costs + exhaustion (LANDED 2026-07-09)

- A swing spends `(stamina_swing + stamina_weight × weapon kg) × the
  attack's stamina column` (attacks.cat gained the fourth number; chop/
  bash 1.5, jab 0.7). A whiff spends too — swinging IS the exertion.
  Casting stays mana's business.
- A step spends `stamina_step` per standing member (part 3's marching
  exertion; the holdoff makes sustained marching a net drain while
  strolling-with-pauses stays neutral). Both routes feed VIT's creep.
- Regen: `stamina_regen + stamina_regen_max × maxStamina` per second
  after `stamina_holdoff` seconds without a spend, in the mana-regen
  tick. Food keeps its instant restore.
- Exhausted (the bar empties): swings still land — penalties, not
  paralysis — at `× exhaust_damage` damage and `× exhaust_pace` interval
  (applied after the normal clamp) until stamina climbs past
  `exhaust_recover × max` (hysteresis). log.exhausted / log.recovered
  member lines; the latch is a live transient, re-derived from the bar
  on load. All nine knobs ride balance.cat → the Balance dialog.

### Phase 5 — death & revive (LANDED 2026-07-09)

0 HP splits into UNCONSCIOUS vs DEAD:

- A member dropping to 0 is **unconscious** (the down state: skipped by
  monsters, acts on nothing). They **stabilize on their own**: after
  `stabilize_time` seconds with no live monster within its own aggro
  range of the party, they wake at `stabilize_health` of max
  (log.member_wakes). Any danger resets the clock; the timer is a live
  transient (a load restarts it).
- **Dead** needs deliberate overkill: a hit landing on a member ALREADY
  at 0 (possible once monsters get area/special attacks) or a single
  blow ≥ `overkill` × maxHealth (log.member_dies replaces the down
  line). Dead members never wake; a future resurrection mechanic
  (altar/spell) is the revive path — out of scope beyond the flag.
  `Character::dead`, save v18 "dead" line (written only when set;
  absent = alive-or-unconscious).
- Party wipe stays as-is (all members at 0, unconscious or dead) — but
  wake-on-stabilize means a wipe now requires the monsters to actually
  finish the job before wandering off; review the latch at feel-test.
- Healing sources (potions, a water heal spell) remain the magic queue's
  business; stabilize keeps death survivable until they land.
- All three knobs (stabilize_time 30, stabilize_health 0.2,
  overkill 1.5) ride balance.cat → the Balance dialog.

### Phase 6 — monster swing animation + poison/bleed (LANDED 2026-07-09)

- **Swing anim:** already covered — the animation thread had baked
  idle/walk/**attack**/die clips into every procedural rig (skeleton
  family, blob, mummy; ModelBaker), and a clip named `attack` auto-
  supports the state, so `attackReq` → DesiredState plays it. skel_human
  carries its authored anim_attack rows. Visual confirmation rides the
  feel-test (a monster swinging at the party is unmissable).
- **Poison/bleed:** `StatusKind::Poison`/`::Bleed` (save tokens
  "poison"/"bleed"; old saves skip unknown kinds — no version bump).
  monsters.cat opt-in per type: `poison = <dps> <seconds> [chance]`
  (ditto `bleed`), rolled on a LANDED melee blow; same-kind reapply
  REFRESHES (the ward-recast rule), poison/bleed/wards stack. The
  effects-aging tick accumulates `magnitude × dt` per member and wounds
  once per frame through `WoundMember(quiet)` — no splat, silent water
  soak, but the one-shot down/death lines still fire. **DoT ticks a
  DOWNED member** (Michael's call): the tick that lands on someone at 0
  is death by the overkill rule — get the poisoned to safety. A DoT down
  also latches the party wipe. Tints ride the school convention (poison
  earth green, bleed fire red); HUD name-band icons, the sheet's Effects
  tab (`effect.*.desc` formats the dps in), and the v14 save lines came
  free. Carriers: the blob envenoms (`poison = 1 20 1`), the lurking
  skeleton opens wounds (`bleed = 2 10 0.5`). log.effect_fades when one
  runs its course. A future serrated weapon reuses the whole mechanism
  from items.cat.

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
