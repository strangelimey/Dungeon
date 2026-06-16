// ============================================================================
// Game/Combat.h — the strike resolver, shared by every source of damage.
//
// Combat is pure data here: a Character's derived accessors (Character.h) and a
// monster's catalog stats (DungeonWorld) both flatten into an AttackProfile +
// DefenseProfile, and ResolveAttack rolls one strike against the supplied RNG.
// Keeping it state-free means spells and item effects reuse the same math when
// they land — they just build the profiles differently.
// ============================================================================
#pragma once

#include <random>

namespace dungeon::game {

// A combatant's offensive profile for one strike (built from attributes, a
// weapon, or a spell's power).
struct AttackProfile {
	float damage = 1.0f;   // base damage on a clean hit
	float accuracy = 0.7f; // 0..1 base chance to land before evasion
};

// A combatant's defensive profile.
struct DefenseProfile {
	float evasion = 0.0f; // 0..1 subtracted from the attacker's accuracy
	float armor = 0.0f;   // flat damage soaked on a hit
};

// One resolved strike.
struct AttackResult {
	bool hit = false;
	float damage = 0.0f; // damage to apply (>= 0; 0 on a miss)
};

// Resolves a single strike with `rng`. Hit chance is (accuracy - evasion)
// clamped to [0.05, 0.95] so nothing is ever a sure thing; on a hit the damage
// is the profile damage jittered +/-15%, minus armor, floored at 1 so a landed
// blow always stings. No game state is touched — the caller applies the result.
AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   std::mt19937& rng);

} // namespace dungeon::game
