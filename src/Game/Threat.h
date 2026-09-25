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
//   offence   = P(hit) x damage / attackcd + each DoT's rate x its uptime,
//               for the better of its two attacks, x kRangedEdge when that
//               one is a shot
//   uptime    = min(1, seconds x P(hit) x chance / attackcd)
//
// A DoT counts as a RATE, not as its total per hit, because every effect a
// monster lands refreshes rather than stacks (effects.cat `stacking`): the
// blob's twenty-second poison, re-landed every 2.2 s, is one point a second
// for as long as the fight lasts, not twenty points a swing. Scoring the total
// put the blob at the top of the whole ranking.
//   toughness = hp x soak x resist / P(it gets hit)
//   threat    = sqrt(offence x toughness)
//
// A kind has up to TWO attacks: its melee blow and, for a ranged archetype
// (skirmisher or caster), the SHOT it throws - a caster's spell bolt, anyone
// else's plain bolt. The first cut scored melee only, which ranked every caster
// near the bottom: the skeleton mage swings a feeble staff, but what it actually
// does is throw fire. The attacks arrive RESOLVED (an Attack is plain numbers)
// because working out what a spell deals needs the spell registry, the damage
// types and the balance knobs, which are all the world's.
// DungeonWorld::ThreatProfile builds them the way the monster's own attacks are
// built, and this module stays pure.
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

#include <optional>
#include <vector>

namespace dungeon::game::threat {

// A damage-over-time effect a landed blow may leave (an on_hit proc).
struct Dot {
	double rate = 0;    // damage per second while it runs
	double seconds = 0; // how long one application runs
	double chance = 1;  // of taking, per landed blow
};

// One way a kind deals damage, as numbers.
struct Attack {
	double damage = 0;   // per landed blow, its `powers` already applied
	double accuracy = 0; // d100 points it attacks with (melee: after the stance)
	std::vector<Dot> dots;
};

// Everything a kind can do to the party.
struct Profile {
	Attack melee;
	std::optional<Attack> shot; // a ranged archetype's bolt; unset for the rest
};

struct Parts {
	double hit = 0;       // chance its better attack lands on the reference party
	double beHit = 0;     // chance the reference party's blow lands on it
	double melee = 0;     // melee damage per second
	double shot = 0;      // shot damage per second before the edge (0 = no shot)
	double offence = 0;   // the better of the two, the edge applied to a shot
	double toughness = 0; // expected raw damage to kill it
	double threat = 0;
};

// The reference party the numbers are measured against.
inline constexpr double kRefDefense = 30.0;  // d100 points of guard
inline constexpr double kRefAccuracy = 60.0; // d100 points of attack
inline constexpr double kRefBlow = 10.0;     // raw damage per landed blow

// What shooting from range is worth on top of the damage itself: a kiter lands
// shots while the party closes the gap, backs off to keep landing them, and is
// out of the front rank's reach while it does. 1.5 is a FIRST CUT (Michael,
// 2026-09-25: a guess until the balance pass). `threat` prints the shot before
// the edge, so what a different value would do can be read straight off it.
inline constexpr double kRangedEdge = 1.5;

// Melee only, straight from the catalog: raw damage, no `powers`, no on-hit
// effects. The fallback for a caller with no world to resolve a Profile with.
Profile FromCatalog(const CatalogEntry& monster);

Parts Of(const CatalogEntry& monster, const Profile& attacks);

} // namespace dungeon::game::threat
