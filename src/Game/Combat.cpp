#include "Game/Combat.h"

namespace dungeon::game {

VerbProfile VerbProfileFor(std::string_view verb) {
	// Feel-test numbers (docs/combat.md Phase 2): fast/precise stab, heavy
	// chop, bash between them (its stun hook is a later phase).
	if (verb == "stab") return {0.8f, 0.05f, 0.8f};
	if (verb == "chop") return {1.3f, -0.05f, 1.25f};
	if (verb == "bash") return {1.15f, -0.05f, 1.2f};
	return {}; // slash / punch / kick / swing / melee — the neutral baseline
}

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
