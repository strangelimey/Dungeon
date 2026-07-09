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
	DamageType type = DamageType::Bash;
	float dmg = 1.0f;  // × the profile damage
	float acc = 0.0f;  // + the profile accuracy
	float pace = 1.0f; // × the swing interval (a whiff pays it too)
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
	float hitFloor = 0.05f, hitCeil = 0.95f; // nothing's ever sure
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

	// The attack table, seeded with the identity defaults; Load overrides the
	// numbers from attacks.cat.
	std::vector<AttackSpec> attacks;

	Balance();

	// The resolver's knob subset, handed to ResolveAttack.
	StrikeRules Strike() const {
		return {hitFloor, hitCeil, damageJitter, woundFloor};
	}

	// The spec for a melee verb; null for unknown/empty (callers use Neutral()).
	const AttackSpec* FindAttack(std::string_view id) const;
	static const AttackSpec& Neutral(); // dmg ×1, acc +0, pace ×1, bash

	// Clamps a SUMMED resist to ±resistClamp — except an authored nature cell
	// at 1.0+, which reaches true immunity (docs/combat.md part 4).
	float ClampResist(float sum, float natureCell) const;

	// balance.cat [formula] knobs + attacks.cat numeric overrides. Missing
	// files/fields keep the defaults, so a project without them still runs.
	void Load(const Catalog& balanceCat, const Catalog& attacksCat);
	// Writes the live values back into the catalogs (the editor's Save path).
	void Save(Catalog& balanceCat, Catalog& attacksCat) const;
};

// The knob fields table: catalog key ↔ Balance member. Drives Load/Save and
// the editor Balance dialog rows (one row per entry — add a knob, add a row).
struct BalanceField {
	const char* key; // balance.cat field name (snake_case)
	float Balance::*value;
};
std::span<const BalanceField> BalanceFields();

// --- school helpers (docs/combat.md parts 1-2) -------------------------------
// A damaging spell's type is its SCHOOL; a school's associated stat is
// earth/fire → INT, air/water → WIL. Form symbols map through their element.
DamageType SchoolDamageType(SpellSymbol school);
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
				  std::string_view owner);

} // namespace dungeon::game
