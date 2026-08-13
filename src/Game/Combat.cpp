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
	// The face travels only when it MEANS something. Reporting the first face
	// unconditionally would hand the consequence layer a number it then has to
	// re-test `fumble` against anyway, giving mishap::Severe two ways to be
	// asked the same question — and 0 is what says "no fumble was recorded".
	if (o.attack.fumble) result.fumbleFace = o.attack.first;
	result.margin = o.margin;

	// A FUMBLE IS AUTOMATIC — "a roll of <= 5 is an automatic fumble", so it
	// decides the exchange rather than merely contributing a low number to it.
	// A veteran with a +90 bonus can still drop his guard, which is the whole
	// reason the rule is worth having: without the override a fumble is
	// invisible to anyone good at fighting.
	//
	// Deliberately HERE and not in Resolve(): the dice module reports what the
	// dice DID, and what a fumble MEANS is a rule of this game. Keeping the two
	// apart is also what leaves RollTest's opposed-roll statistics describing
	// the roll rather than the ruleset laid over it.
	//
	// The attacker's fumble wins: a swing that goes that badly cannot land even
	// against a guard that went equally badly.
	bool hit = o.hit;
	if (o.attack.fumble) {
		hit = false;
	} else if (o.defense.fumble) {
		hit = true;
		result.defenderFumbled = true;
	}
	if (!hit) return result; // missed, blocked, or fumbled away

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
	// PIERCE (weapons.cat `crit = pierce`): a critical with the right edge goes
	// UNDER the armour, so soak is not subtracted at all. Resist still answers
	// it — soak is a thing you WEAR and a gap can be found in it, while a
	// resist is what the target IS and no edge finds a gap in that.
	const float soak = (result.crit && atk.pierceOnCrit) ? 0.0f : def.soak;
	float dmg =
		(atk.damage * marginMul * jitter(rng) - soak) * (1.0f - def.resist);
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
