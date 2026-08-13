// ============================================================================
// Game/Resource.cpp — see Resource.h.
// ============================================================================
#include "Game/Resource.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace dungeon::game::resource {

namespace {
// Indexed by Kind — health, stamina, mana.
constexpr const char* kSkillIds[] = {kConstitution, kConditioning, kAttunement};
static_assert(std::size(kSkillIds) == static_cast<size_t>(Kind::Count),
			  "one skill per resource");
} // namespace

const char* SkillId(Kind kind) {
	const size_t i = static_cast<size_t>(kind);
	return i < static_cast<size_t>(Kind::Count) ? kSkillIds[i] : "";
}

const Rules& PoolRules::For(Kind kind) const {
	switch (kind) {
	case Kind::Stamina: return stamina;
	case Kind::Mana: return mana;
	default: return health; // Health, and anything out of range
	}
}

// THE ZERO-CAP RULE (see the header). CurveValue would answer a non-positive
// cap with the straight line `slope * x`, which is the right reading for a
// curve in general and the wrong one for a resource: it would let a practice
// add maximum without limit. Here a cap of zero is an authored statement that
// this skill does not feed this resource, so nothing is added.
//
// The slope is left to CurveValue, which already answers a non-positive one
// with zero — two guards for the same "switched off" idea would be two places
// to disagree.
float SkillTerm(const CurveRules& rules, float skillLevel) {
	if (rules.cap <= 0.0f) return 0.0f;
	return CurveValue(skillLevel, rules);
}

float Contribution(const Rules& rules, float aptitude, float skillLevel) {
	// NOT floored — this is the raw term, and the save loader subtracts it to
	// recover a base. Flooring here would make that subtraction lie.
	return rules.perAptitude * aptitude + SkillTerm(rules.skillMax, skillLevel);
}

float Maximum(const Rules& rules, float base, float aptitude, float skillLevel) {
	// A pool cannot be negative. It CAN be zero, and that is a real state (a
	// member with no aptitude for magic simply has no mana), so this floors
	// rather than clamping to some minimum nobody authored.
	return std::max(0.0f, base + Contribution(rules, aptitude, skillLevel));
}

float RegenPerSec(const Rules& rules, const CurveRules& statCurve,
				  float aptitude, float maximum, float skillLevel) {
	const float rate = rules.regenBase +
					   rules.regenPerAptitude * CurveValue(aptitude, statCurve) +
					   rules.regenPerMax * maximum +
					   SkillTerm(rules.skillRegen, skillLevel);
	// The stat curve is odd-symmetric about its baseline, so a poor aptitude
	// contributes a NEGATIVE term — deliberately, since a feeble constitution
	// should recover more slowly than an average one. It must not be able to
	// drive the whole rate below zero, though: regeneration that drains the
	// pool is a different mechanic (a DoT) and belongs in the effects pipeline
	// where everything else that hurts you lives.
	return std::max(0.0f, rate);
}

} // namespace dungeon::game::resource
