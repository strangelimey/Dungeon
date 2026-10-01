// ============================================================================
// Game/Power.h - a monster kind's POWER: the one number that says how strong it
// is (docs/tool-refinement-plan.md Phase 2).
//
// Power is DERIVED - it is the kind's threat (Game/Threat.h: offence against
// toughness, measured on a reference party) - and an author may OVERRIDE it
// with a `power = <n>` field on the monsters.cat entry (Michael: "derived,
// overridable"). The override is for the kind the formula misjudges: a monster
// whose danger is a trick the stats do not show. It moves everything that ranks
// by power, the generator's difficulty pick included, which is the point.
//
// A BAND puts the number on a five-step scale a person can read at a glance:
// which fifth of THIS PROJECT'S range of powers the kind falls in. Relative on
// purpose - "a 4" means strong for this world's monsters, so the scale needs no
// retuning when a world's monsters are all feeble or all fearsome - and linear,
// so a kind far stronger than the rest stands alone at the top instead of being
// dealt a share of the bands by rank.
//
// Pure (no catalog, no world), so RollTest links it - the Defense.h bargain.
// ============================================================================
#pragma once

namespace dungeon::game::power {

inline constexpr int kBands = 5;

// The power a kind has: the authored override when there is one (> 0), else the
// derived value. Zero or less is "not set" - no real kind is authored as
// harmless, and a derived 0 (a kind that never attacks) stays derivable.
double Resolve(double derived, double authored);

// The spread of a set of powers, for Band.
struct Range {
	double lo = 0.0;
	double hi = 0.0;
	bool any = false;
	void Add(double p);
};

// 1..kBands: which fifth of `range` the power falls in. The top edge is band 5,
// not a sixth; a power outside the range clamps. A range with no width (one
// kind, or all equal) puts everything in the middle band - there is no stronger
// or weaker to speak of.
int Band(double p, const Range& range);

} // namespace dungeon::game::power
