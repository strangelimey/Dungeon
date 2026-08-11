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
	float acc = 0.0f;  // + the profile accuracy
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
	float accBase = 0.55f;      // the to-hit line (stat term is ALWAYS DEX)
	float accStat = 0.02f;
	float accSkill = 0.02f;
	float hitFloor = 0.05f, hitCeil = 0.95f; // legacy, unused by the opposed roll
	// The opposed roll (docs/damage-system.md). roll_scale bridges the old
	// 0..1 accuracy/evasion onto d100 points and retires with them in P3.
	float rollScale = 40.0f;
	float critThreshold = 95.0f;
	float fumbleThreshold = 5.0f;
	float marginDamage = 0.01f; // damage multiplier per point of margin
	float marginCap = 3.0f;     // ceiling on that multiplier
	float resistClamp = 0.8f;   // max summed resist (nature 1.0 = immunity)
	float woundFloor = 1.0f;    // a landed blow stings
	float speedBase = 1.15f;    // interval = speed × (speedBase − speedStat×DEX)
	float speedStat = 0.015f;
	float intervalMin = 0.6f, intervalMax = 2.0f;
	float spellStat = 0.01f;       // % spell power per point of statAvg
	float stoneskinResist = 0.05f; // physical resist per point of ward magnitude
	float creepRate = 0.04f;       // stat creep per skill-XP
	float vitExertion = 0.02f;     // VIT creep per stamina point spent
	float kHealth = 1.0f, kStamina = 1.0f, kMana = 1.0f; // resource maxima per stat point
	// Stamina costs + exhaustion (docs/combat.md Phase 4). A swing spends
	// (stamina_swing + stamina_weight × weapon kg) × attack.stam; a step
	// spends stamina_step per standing member. Regen runs at stamina_regen +
	// stamina_regen_max × maxStamina per second after stamina_holdoff seconds
	// without a spend. Hitting 0 latches EXHAUSTED (damage × exhaust_damage,
	// pace × exhaust_pace) until stamina recovers past exhaust_recover of max.
	float staminaSwing = 1.0f;
	float staminaWeight = 0.4f;
	float staminaStep = 0.1f;
	float staminaRegen = 0.5f;
	float staminaRegenMax = 0.02f;
	float staminaHoldoff = 1.5f;
	float exhaustDamage = 0.5f;
	float exhaustPace = 1.5f;
	float exhaustRecover = 0.1f;
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
		r.hitFloor = hitFloor;
		r.hitCeil = hitCeil;
		r.damageJitter = damageJitter;
		r.woundFloor = woundFloor;
		r.rollScale = rollScale;
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

	// Clamps a SUMMED resist to ±resistClamp — except an authored nature cell
	// at 1.0+, which reaches true immunity (docs/combat.md part 4).
	float ClampResist(float sum, float natureCell) const;

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
