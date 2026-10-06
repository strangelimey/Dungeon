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
	if (held == 0.0f) return 0.0f; // the whole skill is on the attack
	// MAX, not sum. Taking the max of the CURVED values rather than curving the
	// max is the same number for any monotonic curve, and says the rule out loud.
	// Applied UNBRANCHED at negative held too — see the header for why the better
	// hand should also be the one that over-commits the furthest.
	return held * std::max(CurveValue(leftLevel, skillCurve),
						   CurveValue(rightLevel, skillCurve));
}

float AttackWeight(float share, const StanceRules& rules) {
	if (share <= 1.0f) return share; // the honest range: the plain trade
	if (rules.exertMax <= 1.0f) return 1.0f; // no over-exertion configured
	// NOT clamped at 1: the dev `guard` command may push a stance past exertMax,
	// and the curve carries on rather than flattening there.
	const float p = (share - 1.0f) / (rules.exertMax - 1.0f);
	return 1.0f + (rules.exertAttackMax - 1.0f) * p * p;
}

float GuardWeight(float share, const StanceRules& rules) {
	const float held = 1.0f - share;
	if (held <= 0.0f) return held; // all-out, or over-exerted: nothing, or less
	return held * (1.0f + (rules.guardDefenseMax - 1.0f) * held * held);
}

float ExertionFumbleFaces(float share, float skillLevel, const StanceRules& rules) {
	if (share <= 1.0f || rules.exertMax <= 1.0f || rules.exertFumble <= 0.0f)
		return 0.0f;
	const float p = std::min(1.0f, (share - 1.0f) / (rules.exertMax - 1.0f));
	// A skilled level of 0 (or less) means "no one is inexperienced": off.
	if (rules.exertSkilledLevel <= 0.0f) return 0.0f;
	const float inexperience =
		std::clamp(1.0f - skillLevel / rules.exertSkilledLevel, 0.0f, 1.0f);
	return rules.exertFumble * p * inexperience;
}

float StanceAttack(float share, float skillLevel, CurveRules skillCurve,
				   const StanceRules& rules) {
	const float skill = CurveValue(skillLevel, skillCurve);
	if (share <= 1.0f) return AttackWeight(share, rules) * skill;
	// Past 1.0: the honest full commitment, plus the over-exerted stretch on at
	// least the floor, so an untrained skill still has something to borrow.
	return skill + (AttackWeight(share, rules) - 1.0f) * std::max(skill, rules.exertFloor);
}

float ExertionPoints(float share, float skillLevel, CurveRules skillCurve,
					 const StanceRules& rules) {
	if (share <= 1.0f) return 0.0f; // not over-exerting: nothing was borrowed
	// The DIFFERENCE against a fully-committed honest swing, written as two calls
	// to the same function the attack roll uses rather than as a separate
	// formula - which is what keeps it right now that the stance no longer
	// scales the skill term linearly past 1.
	return StanceAttack(share, skillLevel, skillCurve, rules) -
		   StanceAttack(1.0f, skillLevel, skillCurve, rules);
}

float Potent(float amount, const ResistTable& potency, DamageType type,
			 float clamp) {
	const float p = std::clamp(potency[type], -clamp, clamp);
	return std::max(0.0f, amount * (1.0f + p));
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

	// An all-out attack guards with NOTHING; an over-exerted one guards with less
	// than nothing (held < 0, so every branch below subtracts). Only the exact
	// zero short-circuits — the sign carries the rest.
	if (in.held == 0.0f) return guard;

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

float Mitigate(float raw, float soak, float resist) {
	// The floor sits on the SOAK's result, before the resist: flooring the
	// product instead would also floor away absorption, and leaving it off is
	// exactly what let a blow under the armour heal (code-review C0).
	return std::max(0.0f, raw - soak) * (1.0f - resist);
}

float SoakMet(float soak, bool crit, bool pierceOnCrit) {
	return crit && pierceOnCrit ? 0.0f : soak;
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
