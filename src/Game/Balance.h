// ============================================================================
// Game/Balance.h — the combat tuning: every knob in the attack formula.
//
// docs/combat.md "The attack formula" is the model; this is its data home.
// Two project catalogs feed one Balance the whole game reads through:
//   * balance.cat  — the knob sheet ([formula] block, key = value floats).
//     kBalanceFields drives load, save, AND the editor Balance dialog rows,
//     so adding a knob is one table row here.
//   * attacks.cat  — the per-attack numbers ([stab] damage/accuracy/speed).
//     An attack's IDENTITY (id + damage type) is the typed C++ table seeded
//     in Balance's constructor — the closed attack list from docs/combat.md —
//     the .cat overrides only the numbers (the spells.cat pattern: recipe =
//     identity, cat = numeric overrides).
// Both are per-PROJECT (whole-dungeon scope), riding the same save /
// synctosource path as every other catalog — Michael's requirement: the
// entire combat model editor-tweakable per dungeon, no rebuild.
// ============================================================================
#pragma once

#include "Game/Combat.h"
#include "Game/Curve.h"
#include "Game/Resource.h"
#include "Game/Spells.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

class Catalog;

// One melee attack (a hand-menu verb). Identity (id + type) is C++; the three
// numbers shade the weapon-derived strike and are attacks.cat data.
struct AttackSpec {
	std::string id;
	// The damage type this verb deals, as an ID — identity, and C++ still owns
	// it (the closed list below), but it names a damagetypes.cat entry rather
	// than an enumerator, so a project can retype a verb or add one dealing a
	// type the engine has never heard of. Resolved to `type` by Load.
	std::string typeId;
	DamageType type{};
	float dmg = 1.0f;  // × the profile damage
	float acc = 0.0f;  // + the attack bonus, in d100 POINTS
	float pace = 1.0f; // × the swing interval (a whiff pays it too)
	float stam = 1.0f; // × the swing's stamina cost (chop exerts, jab doesn't)
};

// Per-monster-type threat (aggro) shading: MULTIPLIERS on the balance.cat
// threat_* globals (1 = the global as-authored), so the Balance dialog stays
// the master dial while each kind keeps its own targeting PERSONALITY. A low
// `threshold` locks a kind onto its tormentor sooner (a single-minded brute);
// a high one keeps it flitting at random longer (an erratic skirmisher).
// `scale` shades how hard it weights damage, `switchMargin` how sticky the
// lock, `decay` how fast the grudge fades. Loaded from monsters.cat
// threat_scale/threat_threshold/threat_switch/threat_decay.
struct ThreatTuning {
	float scale = 1.0f;
	float threshold = 1.0f;
	float switchMargin = 1.0f;
	float decay = 1.0f;
};

struct Balance {
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
	// resource::Rules by Resource() below — the ArmorRules idiom — so the
	// arithmetic can live in a pure TU that RollTest links.
	//
	// EVERY NUMBER BELOW IS A FIRST CUT except the two stamina knobs that were
	// already authored: the defaults are chosen so a NOVICE (all stats 10, all
	// skills 0) regenerates stamina at exactly the rate they always did, while
	// mana slows sharply from its old 1.2/sec and health regenerates at all for
	// the first time. The ordering the model asks for — stamina > mana > health
	// at equal investment — is a property of these values and not of the code,
	// so it is checked rather than assumed (tools/RollTest).
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

	// The attack table, seeded with the identity defaults; Load overrides the
	// numbers from attacks.cat.
	std::vector<AttackSpec> attacks;

	Balance();

	// The resolver's knob subset, handed to ResolveAttack.
	StrikeRules Strike() const {
		StrikeRules r;
		r.damageJitter = damageJitter;
		r.woundFloor = woundFloor;
		r.critThreshold = critThreshold;
		r.fumbleThreshold = fumbleThreshold;
		r.marginDamage = marginDamage;
		r.marginCap = marginCap;
		return r;
	}

	// The spec for a melee verb; null for unknown/empty (callers use Neutral()).
	const AttackSpec* FindAttack(std::string_view id) const;
	// dmg ×1, acc +0, pace ×1, bash. No longer static: its damage type is a
	// LOOKUP now, so it needs the book Load resolved against — a file-static
	// would have to guess an index, and index 0 is whatever the project happens
	// to list first.
	const AttackSpec& Neutral() const { return m_neutral; }

	// One armor class's numbers, gathered so the defense maths reads as one
	// lookup rather than three parallel switch statements.
	struct ArmorRules {
		float penalty = 0.0f, floor = 0.0f, strength = 0.0f, learn = 1.0f;
		// What a skill may ever claw back — the curve's cap.
		float Offsettable() const { return std::max(0.0f, penalty - floor); }
	};
	ArmorRules Armor(ArmorClass c) const;

	// One resource's knobs, gathered — the same idiom, and for the same reason:
	// the arithmetic lives in a pure TU (Game/Resource.h) that RollTest links,
	// and this is the adapter that feeds it. The curve FORM is the shared skill
	// form; only the slope and cap differ per resource, which is the bargain
	// AvoidCurve already makes.
	resource::Rules Resource(resource::Kind kind) const;
	// All three at once — what Character::RecomputeMaxima takes.
	resource::PoolRules Resources() const;

	// The two contribution curves, assembled from the knobs above.
	CurveRules SkillCurve() const {
		return {static_cast<CurveForm>(static_cast<int>(skillCurve)), skillBonus,
				skillCap, 0.0f};
	}
	// The avoid skill's curve: its own slope and ceiling, the shared form.
	CurveRules AvoidCurve() const {
		return {static_cast<CurveForm>(static_cast<int>(skillCurve)), avoidSlope,
				avoidCap, 0.0f};
	}
	CurveRules StatCurve() const {
		return {static_cast<CurveForm>(static_cast<int>(statCurve)), statBonus,
				statCap, statBaseline};
	}

	// Clamps a SUMMED resist to ±resistClamp — except an authored nature cell
	// at 1.0+, which reaches true immunity (docs/combat.md part 4).
	float ClampResist(float sum, float natureCell) const;
	// Scales `amount` by the attacker's potency in `type` — THE one place the
	// attack-side axis is applied, so every source of damage gets it the same way
	// and none can quietly skip it. A cell of 0 is ordinary, positive is potent,
	// negative is feeble; the sum is clamped to ±potencyClamp, and the result never
	// goes below zero (a deeply feeble blow does nothing, it does not heal).
	float Potent(float amount, const ResistTable& potency, DamageType type) const;

	// balance.cat [formula] knobs + attacks.cat numeric overrides. Missing
	// files/fields keep the defaults, so a project without them still runs.
	void Load(const Catalog& balanceCat, const Catalog& attacksCat,
			  const DamageTypeBook& types);
	// Writes the live values back into the catalogs (the editor's Save path).
	void Save(Catalog& balanceCat, Catalog& attacksCat) const;

private:
	AttackSpec m_neutral;
};

// The knob fields table: catalog key ↔ Balance member. Drives Load/Save and
// the editor Balance dialog rows (one row per entry — add a knob, add a row).
struct BalanceField {
	const char* key; // balance.cat field name (snake_case)
	float Balance::*value;
};
std::span<const BalanceField> BalanceFields();

// --- school helpers (docs/combat.md parts 1-2) -------------------------------
// A school's associated stat is earth/fire → INT, air/water → WIL.
// (SchoolDamageType moved to Combat.h: it is a fact about damage types rather
// than a knob, and the effects module asks it without knowing Balance exists.)
const std::vector<std::string>& SchoolStats(SpellSymbol school);
// The unarmed source's associated stats ({"strength"}).
const std::vector<std::string>& UnarmedStats();

// --- catalog field parsing ----------------------------------------------------
// "str, dex" → {"strength", "dexterity"} (full names pass through; unknown
// tokens are dropped with a warning naming `owner`).
std::vector<std::string> ParseStatList(std::string_view spec,
									   std::string_view owner);
// "pierce 0.5, slash 0.25, bash -0.5" → the cells named (others untouched).
void ParseResists(std::string_view spec, ResistTable& out,
				  std::string_view owner, const DamageTypeBook& types);

} // namespace dungeon::game
