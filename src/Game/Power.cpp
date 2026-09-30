// ============================================================================
// Game/Power.cpp - see Power.h.
// ============================================================================
#include "Game/Power.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game::power {

double Resolve(double derived, double authored) {
	return authored > 0.0 ? authored : std::max(0.0, derived);
}

void Range::Add(double p) {
	if (!any) {
		lo = hi = p;
		any = true;
		return;
	}
	lo = std::min(lo, p);
	hi = std::max(hi, p);
}

int Band(double p, const Range& range) {
	const double width = range.hi - range.lo;
	if (!range.any || width <= 1e-9) return (kBands + 1) / 2;
	const double t = std::clamp((p - range.lo) / width, 0.0, 1.0);
	return std::min(kBands, 1 + static_cast<int>(std::floor(t * kBands)));
}

} // namespace dungeon::game::power
