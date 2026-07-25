// ============================================================================
// Game/Combat.cpp — see Combat.h.
// ============================================================================
#include "Game/Combat.h"

namespace dungeon::game {

namespace {
// Record/catalog tokens, indexed by DamageType. Append with the enum.
constexpr const char* kDamageTypeIds[kDamageTypeCount] = {
	"slash", "pierce", "bash", "fire", "earth", "air", "water"};
} // namespace

const char* DamageTypeId(DamageType type) {
	return kDamageTypeIds[static_cast<size_t>(type)];
}

bool ParseDamageType(std::string_view token, DamageType& out) {
	for (size_t i = 0; i < kDamageTypeCount; ++i)
		if (token == kDamageTypeIds[i]) {
			out = static_cast<DamageType>(i);
			return true;
		}
	return false;
}

AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   const StrikeRules& rules, std::mt19937& rng) {
	AttackResult result;

	float chance = atk.accuracy - def.evasion;
	if (chance < rules.hitFloor) chance = rules.hitFloor;
	if (chance > rules.hitCeil) chance = rules.hitCeil;

	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	if (roll(rng) > chance) return result; // miss

	std::uniform_real_distribution<float> jitter(1.0f - rules.damageJitter,
												 1.0f + rules.damageJitter);
	float dmg = (atk.damage * jitter(rng) - def.soak) * (1.0f - def.resist);
	// A landed blow always stings — but only a blow that got THROUGH. At resist
	// 1 the result is exactly nothing (a fire golem takes no fire), and past 1
	// it goes NEGATIVE: the target drinks the element and is healed by it.
	// Flooring unconditionally, as this once did, quietly turned immunity into
	// "one point every time" and made absorption impossible to express.
	if (dmg > 0.0f && dmg < rules.woundFloor) dmg = rules.woundFloor;

	result.hit = true;
	result.damage = dmg;
	return result;
}

DamageType SchoolDamageType(SpellSymbol school) {
	switch (school) {
	case SpellSymbol::Fire: return DamageType::Fire;
	case SpellSymbol::Earth: return DamageType::Earth;
	case SpellSymbol::Air: return DamageType::Air;
	case SpellSymbol::Water: return DamageType::Water;
	default: return DamageType::Bash; // form symbols never deal damage alone
	}
}

} // namespace dungeon::game
