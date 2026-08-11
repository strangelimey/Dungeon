// ============================================================================
// Game/Curve.cpp — see Curve.h.
// ============================================================================
#include "Game/Curve.h"

#include <cmath>

namespace dungeon::game {

namespace {
constexpr const char* kFormIds[] = {"hyperbolic", "exponential", "logarithmic"};

// The positive half of each shape. All three are built to share one meaning
// for `slope`: the derivative AT ZERO is exactly `slope`, so the "+5 a level"
// figure means the same thing whichever form is picked and the forms can be
// compared on the graph without re-tuning.
float PositiveHalf(float x, const CurveRules& r) {
	switch (r.form) {
	case CurveForm::Hyperbolic:
		// cap * x / (x + cap/slope): rises at `slope`, approaches `cap`.
		return r.cap * x / (x + r.cap / r.slope);
	case CurveForm::Exponential:
		// cap * (1 - e^(-slope x / cap)): same start, harder shoulder.
		return r.cap * (1.0f - std::exp(-r.slope * x / r.cap));
	case CurveForm::Logarithmic:
	default:
		// cap * ln(1 + slope x / cap): same start, no ceiling — it passes the
		// cap and keeps going, ever more slowly.
		return r.cap * std::log(1.0f + r.slope * x / r.cap);
	}
}
} // namespace

const char* CurveFormId(CurveForm form) {
	const size_t i = static_cast<size_t>(form);
	return i < static_cast<size_t>(CurveForm::Count) ? kFormIds[i] : kFormIds[0];
}

bool ParseCurveForm(std::string_view token, CurveForm& out) {
	for (size_t i = 0; i < static_cast<size_t>(CurveForm::Count); ++i)
		if (token == kFormIds[i]) {
			out = static_cast<CurveForm>(i);
			return true;
		}
	return false;
}

float CurveValue(float x, const CurveRules& rules) {
	// A non-positive slope means "this term is switched off" — an honest way to
	// take stats out of the formula entirely while tuning, and it also guards
	// the division below.
	if (rules.slope <= 0.0f) return 0.0f;
	// A non-positive cap has no meaningful shape; fall back to the straight
	// line the slope describes rather than dividing by it.
	const float d = x - rules.baseline;
	if (rules.cap <= 0.0f) return rules.slope * d;

	// Odd symmetry about the baseline: the same shape mirrored below it, so a
	// stat of 4 is as much a penalty as 16 is a bonus.
	return d >= 0.0f ? PositiveHalf(d, rules) : -PositiveHalf(-d, rules);
}

} // namespace dungeon::game
