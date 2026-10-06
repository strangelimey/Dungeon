// ============================================================================
// Game/Balance.h — the combat tuning: every knob in the attack formula.
//
// docs/combat.md "The attack formula" is the model; this is its data home.
// Two project catalogs feed one Balance the whole game reads through:
//   * balance.cat  — the knob sheet ([formula] block, key = value floats).
//     kBalanceFields drives load, save, AND the editor Balance dialog rows,
//     so adding a knob is a member of BalanceKnobs (Game/BalanceKnobs.h, the
//     defaults, pure) plus one row in that table.
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

#include "Game/BalanceKnobs.h"
#include "Game/Combat.h"
#include "Game/Curve.h"
#include "Game/Defense.h" // defense::StanceRules
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

// The knob sheet itself - every float, its default and its reasoning - is
// BalanceKnobs (Game/BalanceKnobs.h), a pure header tools/RollTest reads.
// Balance adds what needs the catalogs: the attack table, Load / Save, and
// the adapters that gather the knobs into the shapes the pure rules take.
struct Balance : BalanceKnobs {
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
	// One supply meter's knobs, gathered the same way.
	resource::SupplyRules SupplyOf(resource::Supply which) const;
	// What conditioning adds to a move speed, as a curve in PACE UNITS.
	CurveRules PaceCurve() const {
		return {static_cast<CurveForm>(static_cast<int>(skillCurve)), paceSlope,
				paceCap, 0.0f};
	}

	// The stance's shape, assembled for Game/Defense.h (the pure TU cannot see
	// Balance, the RollTest wall).
	defense::StanceRules Stance() const {
		return {exertMax,   exertAttackMax, guardDefenseMax,
				exertFloor, exertFumble,    exertSkilledLevel};
	}

	// The two contribution curves, assembled from the knobs (BalanceKnobs).
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
// A thrown non-weapon's associated stats ({"strength", "dexterity"}): a throw is
// arm and eye. FULL ids, like every list here (code-review C36: the throw's own
// abbreviated list was the one that printed as `stat.str`).
const std::vector<std::string>& ThrowStats();

// --- the bare hand's attacks ---------------------------------------------------
// Punch and kick: every hand offers them whatever it holds (the hand menu's
// Combat group), and they swing, and train, UNARMED whatever it holds - the fist,
// not the key in it (code-review C39). One list, read by the menu and the swing.
std::span<const std::string_view> UnarmedAttacks();
bool IsUnarmedAttack(std::string_view verb);

// --- catalog field parsing ----------------------------------------------------
// "str, dex" → {"strength", "dexterity"} (full names pass through; unknown
// tokens are dropped with a warning naming `owner`).
std::vector<std::string> ParseStatList(std::string_view spec,
									   std::string_view owner);
// "pierce 0.5, slash 0.25, bash -0.5" → the cells named (others untouched).
void ParseResists(std::string_view spec, ResistTable& out,
				  std::string_view owner, const DamageTypeBook& types);

} // namespace dungeon::game
