// ============================================================================
// Game/BalanceKnobs.h - the combat knob sheet and its DEFAULTS, pure.
//
// Every number in the attack formula (docs/combat.md "The attack formula") is a
// knob: a float with its default here, overridden per project by balance.cat's
// [formula] block (Balance::Load) and edited live in the editor's Balance dialog
// (kBalanceFields in Balance.cpp gives each one its catalog key).
//
// WHY THE SHEET IS ITS OWN HEADER (code-review C423). tools/RollTest cannot link
// Balance.cpp - the catalog reader would drag the file layer in behind it - so
// it used to RETYPE the defaults it measured against, and they drifted: its
// strike table's "a monster vs a party member" row still used a defense_base of
// 25 long after the knob became 45. This header includes nothing, so RollTest
// reads kBalanceDefaults instead and cannot drift; and the pure rule structs
// take their own defaults from it too (defense::StanceRules, StrikeRules), which
// had been a second and a third copy of the same numbers.
//
// Keep it PURE: plain floats, their defaults and their reasoning. Anything that
// needs a catalog, a string or another Game header belongs on Balance, which
// derives from this and adds the attack table, Load / Save and the adapters.
// ============================================================================
#pragma once

namespace dungeon::game {

// The knob sheet. Balance derives from it, so every `balance.defenseBase` in
// the game reads the member it always did.
struct BalanceKnobs {
	// --- the knob sheet (docs/combat.md part 5; all driven by BalanceFields) --
	float unarmedBase = 4.0f;   // fist "weapon damage"
	float unarmedSpeed = 1.4f;  // fist "weapon speed" (seconds, before DEX)
	float statDamage = 0.25f;   // damage per point of statAvg
	float skillDamage = 0.08f;  // damage multiplier per skill level
	float damageJitter = 0.15f; // ± roll on every hit
	// The opposed roll (docs/damage-system.md). roll_scale bridges the old
	// 0..1 accuracy/evasion onto d100 points and retires with them in P3.
	float critThreshold = 95.0f;
	float fumbleThreshold = 5.0f;
	float marginDamage = 0.01f; // damage multiplier per point of margin
	float marginCap = 3.0f;     // ceiling on that multiplier

	// --- the contribution curves (Game/Curve.h) -----------------------------
	// Skill and stat each turn a live value into d100 POINTS through a
	// diminishing-returns curve. `*_bonus` is the rise at the origin (the
	// Rolemaster "+5 a level"), `*_cap` the asymptote, `*_curve` the shape as
	// a CurveForm index. The Balance dialog draws both curves live, against
	// the ~41-point dice deviation RollTest measured — which is the number
	// that says whether a difference in skill can actually beat the noise.
	float skillCurve = 0.0f; // CurveForm index (0 = hyperbolic)
	float skillBonus = 5.0f;
	float skillCap = 120.0f;
	// Stats taper too, and BASELINE 10 makes an average stat worth nothing
	// while a poor one is a real penalty. Bounded far below skill on purpose:
	// unbounded skill against an unbounded stat would make one of them
	// decoration, and Rolemaster kept stats in roughly -25..+35 for the same
	// reason.
	float statCurve = 0.0f;
	float statBonus = 2.0f;
	float statCap = 35.0f;
	float statBaseline = 10.0f;
	// --- armor (docs/damage-system.md) --------------------------------------
	// Per WEIGHT CLASS, in d100 points. `penalty` is what the armor costs your
	// defense roll before any training; `floor` is the part training can NEVER
	// reach past, so the difference is all a skill may ever claw back. The
	// floor is what keeps the choice a choice: no amount of practice makes
	// plate as evadable as leather.
	//
	// The floor is ENFORCED BY THE CURVE rather than by a clamp — the offset is
	// CurveValue with its cap set to (penalty - floor), and a hyperbolic curve
	// approaches its cap without reaching it. The rule falls out of machinery
	// that is already tested.
	float armorLightPenalty = 10.0f, armorLightFloor = 3.0f;
	float armorMediumPenalty = 25.0f, armorMediumFloor = 10.0f;
	float armorHeavyPenalty = 45.0f, armorHeavyFloor = 20.0f;
	// Points of offset per level at the START of the curve; it tapers to the
	// class's own ceiling from there.
	float armorOffsetSlope = 2.0f;
	// The STRENGTH each class asks for, and what falling short costs: more
	// points off the defense roll, and a much steeper stamina bill on every
	// swing and every step. The same story told twice — easier to hit AND
	// quickly spent.
	float armorLightStr = 8.0f, armorMediumStr = 11.0f, armorHeavyStr = 14.0f;
	float armorShortPenalty = 4.0f;  // + points per point of STR short
	float armorShortStamina = 0.15f; // + fraction of stamina cost per point
	// How fast each class trains, relative to the usual rate: plate is harder
	// to learn to live in.
	float armorLightLearn = 1.0f, armorMediumLearn = 0.7f, armorHeavyLearn = 0.45f;

	// A party member's INNATE defense in d100 points — what a bare novice is
	// worth before any training. It is a real term rather than the stopgap it
	// began as: monsters attack at 60-75 points, so a floor much under this
	// leaves a fresh character hit almost every swing, and armor (which SPENDS
	// defense to buy soak) had nothing to spend.
	float defenseBase = 45.0f;
	// The AVOID skill's own curve — the unarmored answer to being swung at.
	// Its own cap rather than the shared skill curve's 120: defense is bounded
	// by what it is defending against, and a term that can reach twice the
	// hardest attack in the game makes a trained dodger untouchable instead of
	// merely hard to hit.
	float avoidSlope = 3.0f;
	float avoidCap = 60.0f;
	float resistClamp = 0.8f;   // max summed resist (nature 1.0 = immunity)
	// THE ATTACKER'S HALF of the type axis (docs/damage-system.md "Two axes"), the
	// mirror of resistClamp: how far a summed POTENCY may push a blow either way.
	// Tighter than the resist clamp on purpose — potency stacks from a weapon AND
	// every worn piece, so it has more sources to pile up than a resist does.
	float potencyClamp = 0.6f;
	float woundFloor = 1.0f;    // a landed blow stings
	float speedBase = 1.15f;    // interval = speed × (speedBase − speedStat×DEX)
	float speedStat = 0.015f;
	float intervalMin = 0.6f, intervalMax = 2.0f;
	float spellStat = 0.01f;       // % spell power per point of statAvg
	float stoneskinResist = 0.05f; // physical resist per point of ward magnitude
	float creepRate = 0.04f;       // stat creep per skill-XP
	// --- the three resources (docs/health-and-healing.md) --------------------
	// APTITUDE and PRACTICE, one pair per pool. `k_<r>` is the aptitude's linear
	// contribution to the MAXIMUM and long pre-dates the rest; everything else
	// here arrived with the health-and-healing model. Assembled into a
	// resource::Rules by Balance::Resource() — the ArmorRules idiom — so the
	// arithmetic can live in a pure TU that RollTest links.
	//
	// EVERY NUMBER BELOW IS A FIRST CUT except the two stamina knobs that were
	// already authored: the defaults are chosen so a NOVICE (all stats 10, all
	// skills 0) regenerates stamina at exactly the rate they always did, while
	// mana slows sharply from its old 1.2/sec and health regenerates at all for
	// the first time. The ordering the model asks for — stamina > mana > health
	// at equal investment — is a property of these values and not of the code,
	// so nothing ASSERTS it: the dev command `regen` MEASURES it on two reference
	// rows (`order ok|BROKEN`) from the AUTHORED knobs (balance.cat), not these
	// defaults, and the `resources` eval suite prints that - a report, not a
	// pass/fail. A retune that breaks the ordering fails nothing; read `regen`.
	float kHealth = 1.0f, kStamina = 1.0f, kMana = 1.0f;
	// What the PRACTICE adds to each maximum: points at the first level, and
	// the asymptote it approaches and never reaches. A cap of 0 switches the
	// term off entirely (resource::SkillTerm) — it does NOT mean "unbounded",
	// which is what the shared curve would otherwise read it as.
	float healthSkillSlope = 1.0f, healthSkillCap = 25.0f;
	float staminaSkillSlope = 1.0f, staminaSkillCap = 25.0f;
	float manaSkillSlope = 1.0f, manaSkillCap = 25.0f;
	// Regeneration, points per second: a flat base, a term per point of the
	// APTITUDE's stat-curve output, a term per point of the pool's own maximum,
	// and the PRACTICE's own curve.
	float healthRegen = 0.15f, healthRegenStat = 0.01f, healthRegenMax = 0.0f;
	float healthRegenSlope = 0.02f, healthRegenCap = 0.45f;
	float manaRegen = 0.3f, manaRegenStat = 0.03f, manaRegenMax = 0.0f;
	float manaRegenSlope = 0.02f, manaRegenCap = 0.5f;
	// How much MANA still trickles while the stamina holdoff is up — the
	// "exerting" row of the state table. Health gets no such knob because it is
	// a flat zero there: you do not knit bone mid-swing.
	float manaExert = 0.25f;
	// TRAINING, in skill XP per point of throughput. A landed blow trains a
	// weapon class at 1.0, which is the unit these sit against. `conditioning_xp`
	// REPLACES the old `vit_exertion` VIT creep — leaving both would be exactly
	// the double-dip the whole model is built to avoid.
	float conditioningXp = 0.3f;  // per stamina point spent
	float attunementXp = 0.25f;   // per mana point spent
	float constitutionXp = 0.4f;  // per health point REGAINED
	// --- SUPPLIES (docs/health-and-healing.md "Food and water") --------------
	// The two meters, out of 100 so a reading is legible as a percentage. Base
	// drain is per second of world time: ~8 hours to empty on food, ~5 on water,
	// because thirst should kill first and the two meters should behave
	// differently rather than being one meter drawn twice.
	//
	// `<s>_cond_slope`/`_cap` are CONDITIONING'S PRICE — extra drain per second,
	// tapering. A cap of 0 switches it off (resource::SkillTerm's rule).
	// `<s>_exertion` is per point of stamina SPENT, and **water is the heavier
	// of the two by design**: sweat is water, so a heavy fight in armour makes
	// you thirsty faster than hungry, and water becomes the supply that decides
	// how deep a stamina-heavy party can go.
	//
	// `hunger_damage`/`thirst_damage` are health per second once the meter is
	// EMPTY. They are the magnitudes of the starving/parched effects, not a
	// separate damage path — see the effect classes.
	float foodMax = 100.0f;
	float foodRate = 0.0035f;
	float foodCondSlope = 0.0002f, foodCondCap = 0.0035f;
	float foodExertion = 0.08f;
	float hungerDamage = 0.5f;
	float waterMax = 100.0f;
	float waterRate = 0.0056f;
	float waterCondSlope = 0.0003f, waterCondCap = 0.0056f;
	float waterExertion = 0.15f;
	float thirstDamage = 0.8f;
	// REST (docs/health-and-healing.md "Rest is a time multiplier"): how much
	// faster the world runs while the party rests. ONE knob, and it multiplies
	// TIME rather than any rate — so health, stamina, mana, food, water, effect
	// timers, monster cooldowns and the AI's own cadence all move together and
	// no second set of resting rates can drift out of step with the ordinary
	// ones. 60 = a minute of dungeon time per second of watching.
	float restScale = 60.0f;
	// PACE (docs/health-and-healing.md "Movement"). What CONDITIONING adds to a
	// member's authored move speed, tapering to its cap — so a trained member
	// walks faster, and the cap is how much faster anyone can ever get.
	//
	// It ADDS to the authored value rather than replacing it: `moveSpeed` is
	// class identity (Sera fleet-footed at 1.2, Tilo the anchor at 0.9) in
	// exactly the way `baseHealth` is, and training should close that gap rather
	// than erase it. A cap of 0 switches the term off (resource::SkillTerm).
	float paceSlope = 0.02f, paceCap = 0.4f;
	// THROWING (ui-updates Phase 10; DungeonWorld_Throw.cpp). Anything held can
	// be thrown by the party LEADER, and a throw IS an attack: the party attack
	// formula (PartyAttackProfile) with the `throwing` skill, the attack the
	// item flies as (its `throw`, a weapon's first command, else `throw`) and a
	// base of the weapon's damage - or, for anything else, throw_base +
	// throw_weight per kg. Michael: its SPEED is the thrower's skill and the
	// thing's weight - throw_speed + throw_speed_skill per level -
	// throw_speed_weight per kg, never under throw_speed_min m/s. It flies
	// throw_range squares, spends throw_stamina + stamina_weight per kg x the
	// attack's stamina, and the thrower waits throw_interval seconds.
	float throwBase = 2.0f;
	float throwWeight = 2.0f;
	float throwSpeed = 9.0f;
	float throwSpeedSkill = 0.4f;
	float throwSpeedWeight = 1.5f;
	float throwSpeedMin = 3.0f;
	float throwRange = 4.0f;
	float throwStamina = 1.0f;
	float throwInterval = 1.0f;
	// Lighting a MAGICAL torch by its own word (the hand menu's Light): it costs
	// the holder torch_light_mana for each of the torch's `power_level`s.
	float torchLightMana = 5.0f;
	// A FLAMMABLE monster (monsters.cat `flammable`) set alight by any fire that
	// lands on it: it burns ignite_burn a second for ignite_seconds.
	float igniteBurn = 2.0f;
	float igniteSeconds = 6.0f;
	// How long a TRACK lasts (lighting-updates 6g): seconds after a monster
	// stepped on a square before its track has faded away (an Earth stone shows
	// the ones within its reach).
	float trackLife = 300.0f;
	// Stamina costs + exhaustion (docs/combat.md Phase 4). A swing spends
	// (stamina_swing + stamina_weight × weapon kg) × attack.stam; a step
	// spends stamina_step per standing member. Regen is the resource model
	// above, held off for stamina_holdoff seconds after any spend. Hitting 0
	// latches EXHAUSTED (damage × exhaust_damage, pace × exhaust_pace) until
	// stamina recovers past exhaust_recover of max.
	float staminaSwing = 1.0f;
	float staminaWeight = 0.4f;
	float staminaStep = 0.1f;
	float staminaRegen = 0.5f;
	float staminaRegenStat = 0.02f;
	float staminaRegenMax = 0.02f;
	float staminaRegenSlope = 0.03f, staminaRegenCap = 0.6f;
	float staminaHoldoff = 1.5f;
	float exhaustDamage = 0.5f;
	float exhaustPace = 1.5f;
	float exhaustRecover = 0.1f;
	// OVER-EXERTION (docs/damage-system.md "Over-exertion"). The stance may be
	// pushed past 1 as far as exert_max, buying attack points at the price of a
	// guard that goes NEGATIVE. Every swing or cast thrown from such a stance is
	// billed exert_cost × the points it bought — out of stamina first, and out of
	// HEALTH for whatever stamina could not cover. exert_cost is the dial the
	// whole mechanic turns on; 3 is a first cut and expected to move.
	float exertCost = 3.0f;
	float exertMax = 2.0f;
	// THE STANCE'S TWO ENDS (Game/Defense.h StanceRules, Michael 2026-09-28):
	// the multiple of the skill term at 100% over-exertion (attack) and at 0%
	// attack (guard). Both curve in with the square of how far toward the end
	// the stance is, so most of the multiple lives in the last stretch.
	float exertAttackMax = 5.0f;
	float guardDefenseMax = 2.0f;
	// The least skill term over-exertion multiplies, in attack points (about a
	// level-1 skill), so an UNTRAINED skill's over-exertion still buys attack
	// and still pays for it (Michael, 2026-09-28: a 100% kick at unarmed 0 was
	// free). 0 restores "an untrained fighter borrows nothing".
	float exertFloor = 5.0f;
	// THE DRUNKEN HAYMAKER (Game/Defense.h ExertionFumbleFaces, Michael
	// 2026-09-28): an over-exerted swing or cast on a skill below
	// exert_skilled_level fumbles on exert_fumble more first faces at 100%
	// over-exertion and level 0 (5 + 45 = 50: half the time), shrinking to
	// nothing at exert_skilled_level. 0 in either switches it off.
	float exertFumble = 45.0f;
	float exertSkilledLevel = 5.0f;
	// FUMBLE CONSEQUENCES (docs/damage-system.md "When it goes wrong"). A fumble
	// fires its source's mild table; at a first face of fumble_severe_face or
	// LESS it fires the severe one as well. At the default thresholds (fumble on
	// 5, severe on 1) that is 5% of swings mild and 1% severe — about one
	// disaster every two or three fights.
	//
	// fumble_recover is the DEFAULT table's number, not a global multiplier: it
	// is the cooldown factor a source that authors no `fumble` of its own gets.
	// A source with its own table never reads it.
	float fumbleSevereFace = 1.0f;
	float fumbleRecover = 2.2f;
	// Death & revive (docs/combat.md Phase 5). 0 HP = UNCONSCIOUS: after
	// stabilize_time seconds with no monster in aggro of the party, the member
	// wakes at stabilize_health of max. DEAD needs deliberate overkill — one
	// blow ≥ overkill × maxHealth, or any hit landing on a member already at
	// 0 — and dead members never wake (resurrection is a future mechanic).
	float stabilizeTime = 30.0f;
	float stabilizeHealth = 0.2f;
	float overkill = 1.5f;
	// COLLISIONS (docs/effects.md): the two blows the WORLD lands, as opposed
	// to anything holding a weapon. Both arrive as Impact bash through the one
	// pipeline, so these are the amount BEFORE armour, Stone Skin and resists
	// answer it — a plated party shrugs a wall off and barely feels a shaft.
	float bumpDamage = 2.0f; // lurching into a wall / door / brazier
	float fallDamage = 6.0f; // a storey's plunge down a pit shaft
	// Threat (aggro). Damage a member deals a monster accrues threat_scale ×
	// damage on that monster; past threat_threshold the monster LOCKS onto the
	// highest-threat member (another member must exceed the locked score by
	// threat_switch to steal it), and all scores drain threat_decay/second, so
	// grudges fade back to the old uniform-random targeting between fights.
	float threatScale = 1.0f;
	float threatThreshold = 15.0f;
	float threatSwitch = 5.0f;
	float threatDecay = 1.0f;
};

// The shipped defaults as one constant: what tools/RollTest measures against,
// and what the pure rule structs' default member initialisers read.
inline constexpr BalanceKnobs kBalanceDefaults{};

} // namespace dungeon::game
