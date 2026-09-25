// ============================================================================
// Game/Threat.h - how dangerous a monster kind is, as one number, DERIVED from
// its monsters.cat stats (docs/level-building.md P4; Michael chose derived over
// an authored tier, so there is no field to keep in step with the stats).
//
// The question it answers: against a REFERENCE PARTY, how much does this kind
// hurt per second (offence), and how much punishment does it take to put down
// (toughness)? Threat is their geometric mean, so a glass cannon and a harmless
// wall both land in the middle and neither dominates the ranking.
//
//   offence   = P(hit) x damage / attackcd
//   toughness = hp x soak x resist / P(it gets hit)
//   threat    = sqrt(offence x toughness)
//
// The rolls are the combat model's own (docs/combat.md): an OPPOSED d100, the
// attacker's accuracy x offense (the stance) against the defender's defense
// plus the accuracy the stance held back. Two opposed d100s differ by a
// triangular distribution, which P() below integrates in closed form.
//
// The reference party is a fixed yardstick, not a claim about any real party:
// what the generator needs is the ORDER of the monsters, and difficulty picks by
// rank, so the absolute numbers only have to be sane. `threat` in the console
// prints every kind's parts, so the ranking can be read before anything is
// tuned against it.
// ============================================================================
#pragma once

#include "Game/Catalog.h"

namespace dungeon::game::threat {

struct Parts {
	double hit = 0;       // chance its blow lands on the reference party
	double beHit = 0;     // chance the reference party's blow lands on it
	double offence = 0;   // expected damage per second
	double toughness = 0; // expected raw damage to kill it
	double threat = 0;
};

// The reference party the numbers are measured against.
inline constexpr double kRefDefense = 30.0;  // d100 points of guard
inline constexpr double kRefAccuracy = 60.0; // d100 points of attack
inline constexpr double kRefBlow = 10.0;     // raw damage per landed blow

Parts Of(const CatalogEntry& monster);

} // namespace dungeon::game::threat
