#include "Game/Combat.h"

namespace dungeon::game {

AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   std::mt19937& rng) {
	AttackResult result;

	float chance = atk.accuracy - def.evasion;
	if (chance < 0.05f) chance = 0.05f;
	if (chance > 0.95f) chance = 0.95f;

	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	if (roll(rng) > chance) return result; // miss

	std::uniform_real_distribution<float> jitter(0.85f, 1.15f);
	float dmg = atk.damage * jitter(rng) - def.armor;
	if (dmg < 1.0f) dmg = 1.0f; // a landed blow always stings

	result.hit = true;
	result.damage = dmg;
	return result;
}

} // namespace dungeon::game
