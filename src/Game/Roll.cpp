// ============================================================================
// Game/Roll.cpp — see Roll.h.
// ============================================================================
#include "Game/Roll.h"

namespace dungeon::game {

Roll RollOpenEnded(const RollRules& rules, std::mt19937& rng) {
	// Guard the die itself: a `sides` of 0 from a mangled catalog would make
	// the distribution degenerate rather than throw, and silently.
	const int sides = rules.sides > 0 ? rules.sides : 100;
	std::uniform_int_distribution<int> die(1, sides);

	Roll r;
	r.first = die(rng);
	r.total = r.first;
	r.fumble = r.first <= rules.fumbleThreshold; // FIRST roll only — see Roll.h

	// Escalate while the LAST face was high enough. Note it is the face that
	// re-triggers, not the running total: a total of 300 does not keep rolling
	// on its own, or nothing would ever stop.
	int face = r.first;
	while (face >= rules.critThreshold) {
		if (r.escalations >= rules.maxEscalations) {
			r.capped = true;
			break;
		}
		face = die(rng);
		r.total += face;
		++r.escalations;
		r.crit = true;
	}
	return r;
}

Opposed Resolve(int attackBonus, int defenseBonus, const RollRules& rules,
				std::mt19937& rng) {
	Opposed o;
	o.attack = RollOpenEnded(rules, rng);

	if (rules.openEndedDefense) {
		o.defense = RollOpenEnded(rules, rng);
	} else {
		// Still a d100, just one that neither escalates nor fumbles.
		RollRules flat = rules;
		flat.critThreshold = rules.sides + 1; // unreachable
		flat.fumbleThreshold = 0;             // unreachable
		o.defense = RollOpenEnded(flat, rng);
	}

	o.attackTotal = attackBonus + o.attack.total;
	o.defenseTotal = defenseBonus + o.defense.total;
	o.margin = o.attackTotal - o.defenseTotal;
	o.hit = o.margin > 0; // a tie goes to the defender — see Roll.h
	return o;
}

} // namespace dungeon::game
