// ============================================================================
// Game/Combat.cpp — see Combat.h.
// ============================================================================
#include "Game/Combat.h"

#include "Game/Roll.h"

namespace dungeon::game {

namespace {
constexpr const char* kArmorClassIds[] = {"none", "light", "medium", "heavy"};
constexpr const char* kArmorSkillIds[] = {"", "light_armor", "medium_armor",
										  "heavy_armor"};
} // namespace

const char* ArmorClassId(ArmorClass c) {
	const size_t i = static_cast<size_t>(c);
	return i < static_cast<size_t>(ArmorClass::Count) ? kArmorClassIds[i]
													 : kArmorClassIds[0];
}

const char* ArmorSkillId(ArmorClass c) {
	const size_t i = static_cast<size_t>(c);
	return i < static_cast<size_t>(ArmorClass::Count) ? kArmorSkillIds[i] : "";
}

bool ParseArmorClass(std::string_view token, ArmorClass& out) {
	for (size_t i = 0; i < static_cast<size_t>(ArmorClass::Count); ++i)
		if (token == kArmorClassIds[i]) {
			out = static_cast<ArmorClass>(i);
			return true;
		}
	return false;
}

AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   const StrikeRules& rules, std::mt19937& rng) {
	AttackResult result;

	// THE OPPOSED ROLL (docs/damage-system.md). Both sides add a d100 to a
	// bonus and the higher total wins; both sides can crit and fumble. This
	// replaced a one-sided (accuracy - evasion) probability check, and the two
	// are not the same shape at all: a probability difference is linear, while
	// an opposed roll turns the same difference into a triangular-ish curve.
	// The bonuses now arrive in points from the contribution curves, which is
	// what let the bridging scale factor go.
	RollRules rr;
	rr.critThreshold = static_cast<int>(rules.critThreshold);
	rr.fumbleThreshold = static_cast<int>(rules.fumbleThreshold);
	rr.maxEscalations = static_cast<int>(rules.maxEscalations);
	const int atkBonus = static_cast<int>(atk.attackBonus);
	const int defBonus = static_cast<int>(def.defenseBonus);

	const Opposed o = Resolve(atkBonus, defBonus, rr, rng);
	result.crit = o.attack.crit;
	result.fumble = o.attack.fumble;
	result.margin = o.margin;
	if (!o.hit) return result; // missed, or blocked

	// THE MARGIN MULTIPLIES. Beating a defense by a hair lands a normal blow;
	// overwhelming it lands a devastating one. Capped: the roll is open-ended,
	// so a lucky swing widens the margin AND the margin scales the damage, and
	// uncapped that product has a very long tail (RollTest measures the extreme
	// margin at ~9x the typical winning one).
	float marginMul = 1.0f + rules.marginDamage * static_cast<float>(o.margin);
	if (marginMul < 1.0f) marginMul = 1.0f; // a 1-point win is still a hit
	if (marginMul > rules.marginCap) marginMul = rules.marginCap;

	std::uniform_real_distribution<float> jitter(1.0f - rules.damageJitter,
												 1.0f + rules.damageJitter);
	float dmg =
		(atk.damage * marginMul * jitter(rng) - def.soak) * (1.0f - def.resist);
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

} // namespace dungeon::game
