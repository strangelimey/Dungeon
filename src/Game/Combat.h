// ============================================================================
// Game/Combat.h — the strike resolver, shared by every source of damage.
//
// Combat is pure data here: the attacker flattens into an AttackProfile (typed
// damage + accuracy), the defender into a DefenseProfile (evasion + soak + the
// resist already resolved for the incoming type), and ResolveAttack rolls one
// strike under the StrikeRules knobs (Balance::Strike()). Keeping it state-free
// means melee, monster blows, and spell bolts reuse the same math — they just
// build the profiles differently. The formula lives in docs/combat.md ("The
// attack formula"); the knobs live in the project's balance.cat (Balance.h).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <random>
#include <string_view>

namespace dungeon::game {

// The seven damage types (docs/combat.md): three physical + the four school
// elements. Every ATTACK (melee verb, spell school) deals exactly one, globally
// — stab is always pierce, whoever swings it.
enum class DamageType : u8 { Slash, Pierce, Bash, Fire, Earth, Air, Water };
inline constexpr size_t kDamageTypeCount = 7;
inline constexpr bool IsPhysical(DamageType t) { return t <= DamageType::Bash; }

// Record/catalog token for a type ("slash", ... "water") and its parse.
const char* DamageTypeId(DamageType type);
bool ParseDamageType(std::string_view token, DamageType& out);

// A defender's per-type resistance cells: the fraction of that type shrugged
// off (0.5 = half), NEGATIVE = vulnerability (-0.5 = half again). Sources
// (nature, equipment, wards) each hold one of these and SUM into the defense;
// the clamp rule lives in Balance::ClampResist.
struct ResistTable {
	std::array<float, kDamageTypeCount> cells{};
	float& operator[](DamageType t) { return cells[static_cast<size_t>(t)]; }
	float operator[](DamageType t) const { return cells[static_cast<size_t>(t)]; }
	void Add(const ResistTable& o) {
		for (size_t i = 0; i < kDamageTypeCount; ++i) cells[i] += o.cells[i];
	}
};

// A combatant's offensive profile for one strike (built from the weapon +
// attack + stats formula, a monster's catalog stats, or a spell's power).
struct AttackProfile {
	float damage = 1.0f;   // base damage on a clean hit (formula-assembled)
	float accuracy = 0.7f; // 0..1 base chance to land before evasion
	DamageType type = DamageType::Bash; // what the defender resists it AS
};

// A combatant's defensive response to ONE incoming strike. The caller resolves
// the per-type resist (summed + clamped) before building this, so the resolver
// stays a pure function of numbers.
struct DefenseProfile {
	float evasion = 0.0f; // 0..1 subtracted from the attacker's accuracy
	float soak = 0.0f;    // small flat damage soaked on a hit (armor)
	float resist = 0.0f;  // resolved resist[type]: fraction shrugged (+) / amplified (−)
};

// The resolver's knobs, filled from Balance (balance.cat) by Balance::Strike().
struct StrikeRules {
	float hitFloor = 0.05f, hitCeil = 0.95f; // nothing's ever sure
	float damageJitter = 0.15f;              // ± roll on every hit
	float woundFloor = 1.0f;                 // a landed blow always stings
};

// One resolved strike.
struct AttackResult {
	bool hit = false;
	float damage = 0.0f; // damage to apply (>= 0; 0 on a miss)
};

// Resolves a single strike with `rng`. Hit chance is (accuracy - evasion)
// clamped to the rules' floor/ceiling; on a hit the damage is jittered, then
// soaked and resisted — final = (rolled − soak) × (1 − resist) — and floored
// so a landed blow always stings. No game state is touched — the caller
// applies the result.
AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   const StrikeRules& rules, std::mt19937& rng);

} // namespace dungeon::game
