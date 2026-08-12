// ============================================================================
// Game/Defense.cpp — see Defense.h.
// ============================================================================
#include "Game/Defense.h"

#include <algorithm>

namespace dungeon::game::defense {

float ArmorPenalty(float floor, float offsettable, CurveRules offsetCurve,
				   float skillLevel, float strengthNeeded, float strength,
				   float shortPenalty) {
	// NOTHING OFFSETTABLE means the penalty IS the floor and no curve may be
	// consulted. This is not defensive tidying — it is load-bearing, and the
	// armor harness found it. The floor rule works by setting the offset curve's
	// CAP to `offsettable`, but CurveValue documents `cap <= 0` as "no meaningful
	// shape, fall back to the straight line the slope describes" — which is right
	// for a stat term switched off and catastrophic here, because a straight line
	// has no ceiling. A class authored with floor == penalty (a designer saying
	// "this cost cannot be trained away at all") then earned an UNBOUNDED offset:
	// at training 200 the penalty came out near -200000, turning that armor into
	// an enormous evasion bonus. Authorable from balance.cat, silent, and fatal
	// to the whole defense roll.
	float penalty = floor;
	if (offsettable > 0.0f) {
		offsetCurve.cap = offsettable; // the floor IS this cap — see the header
		penalty += offsettable - CurveValue(skillLevel, offsetCurve);
	}

	// Too weak for it: easier to hit AND quickly spent (the stamina half lives in
	// SpendStamina).
	const float short_ = strengthNeeded - strength;
	if (short_ > 0.0f) penalty += short_ * shortPenalty;
	return penalty;
}

float HandGuard(float held, CurveRules skillCurve, float leftLevel,
				float rightLevel) {
	if (held <= 0.0f) return 0.0f; // all-out attack guards with nothing
	// MAX, not sum. Taking the max of the CURVED values rather than curving the
	// max is the same number for any monotonic curve, and says the rule out loud.
	return held * std::max(CurveValue(leftLevel, skillCurve),
						   CurveValue(rightLevel, skillCurve));
}

GuardKind GuardKindFor(TypeFacts facts) {
	// School FIRST: knowing fire is what turns fire aside, and that holds even for
	// a type a project also marked physical.
	if (facts.hasSchool) return GuardKind::Magical;
	if (facts.physical) return GuardKind::Physical;
	return GuardKind::Neither;
}

float Guard(const GuardInputs& in) {
	// The innate floor plus DEX.
	float guard = in.base + CurveValue(in.dexterity, in.statCurve);

	// Wearing anything costs you the roll and pays you back in soak; wearing
	// NOTHING is the only way `avoid` applies at all. The two are exclusive by
	// construction here — an armored defender's avoidLevel is never read, so
	// training it while armored can never leak into the roll.
	if (in.worn == ArmorClass::None)
		guard += CurveValue(in.avoidLevel, in.avoidCurve);
	else
		guard -= in.armorPenalty;

	if (in.held <= 0.0f) return guard; // all-out attack guards with nothing

	switch (in.kind) {
	case GuardKind::Magical:
		// The hands play NO part. Worth stating as code rather than as a comment
		// beside a hand loop, which is what it used to be — and that loop once
		// computed the same number twice and took the max of it with itself.
		return guard + in.held * CurveValue(in.schoolLevel, in.skillCurve);
	case GuardKind::Physical:
		return guard +
			   HandGuard(in.held, in.skillCurve, in.leftLevel, in.rightLevel);
	case GuardKind::Neither:
		break;
	}
	return guard;
}

Lesson LessonFrom(bool rolled, bool hit, ArmorClass worn, float soak) {
	if (!rolled) return Lesson::Nothing; // never evaded, never turned
	if (!hit) {
		// A miss teaches avoidance — but only to someone who avoided it rather
		// than someone whose armor was simply not tested.
		return worn == ArmorClass::None ? Lesson::Avoid : Lesson::Nothing;
	}
	// A landed blow only teaches the armor something if the armor was there to
	// blunt it, and actually blunted something.
	if (worn == ArmorClass::None || soak <= 0.0f) return Lesson::Nothing;
	return Lesson::Armor;
}

} // namespace dungeon::game::defense
