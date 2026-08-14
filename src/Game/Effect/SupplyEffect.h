// ============================================================================
// Game/Effect/SupplyEffect.h — starving and parched: what an empty meter does.
//
// Michael's call (docs/health-and-healing.md, question 2): running out of food
// or water KILLS. That is what makes supplies the failure condition of a run
// rather than a nuisance meter — a dungeon can be lost to logistics with every
// monster on the level still alive.
//
// WHY THESE ARE EFFECTS AND NOT A BRANCH IN THE SUPPLY TICK, which is the whole
// design decision here. Draining health directly from the tick would have meant
// writing a second damage path, and everything on it would have had to be
// re-derived: what resists it, what a ward does about it, what happens when it
// lands on someone already unconscious. As effects they inherit all of that:
//
//   * the bite arrives as a Tick through fx::Deal, so it is one pipeline with
//     everything else that hurts you (docs/effects.md);
//   * a Tick on a DOWNED member kills under the existing overkill rule — which
//     IS "starvation can finish you", with no new death path written;
//   * the HUD strip, the sheet's Effects tab and the save round-trip are free.
//
// They are DoTs in every respect but one: a DoT runs out, and these do not.
// Nothing here implements that — the SUPPLY TICK owns it, topping `timeLeft` up
// while the meter is empty and erasing the effect when it is not. So the state
// lives in the meter, which is the thing that is actually true, and the effect
// is only its shadow. There is deliberately no "permanent effect" concept: one
// would need a rule for what clears it, and the meter already is that rule.
// ============================================================================
#pragma once

#include "Game/Effect/DotEffect.h"

namespace dungeon::game::fx {

// Empty food. Slower than thirst, and named for the state rather than the
// meter: the player reads "Brand is starving", not "food 0".
class StarvingEffect : public DotEffect {
public:
	StarvingEffect();
};

// Empty water. THE HARDER OF THE TWO, and the one you meet first — its meter
// drains faster and its damage is larger, so a run that neglects supplies dies
// of thirst. That ordering is the reason the two are separate effects at all
// rather than one "deprived".
class ParchedEffect : public DotEffect {
public:
	ParchedEffect();
};

} // namespace dungeon::game::fx
