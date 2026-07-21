// ============================================================================
// Game/Spell/Farsight.h — Far Sight (air,sight): air sees DEEP.
//
// Air's peek carries down the row — the ghosting pierces successive wall
// blocks ahead, not just the adjacent one (magnitude = how many cells deep).
// The one Sight that beats the one-block base rule; distance is air's identity.
// Growth (docs/spells.md): depth with air power.
// ============================================================================
#pragma once

#include "Game/Spell/SightSpell.h"

namespace dungeon::game::spells {

class Farsight : public SightSpell {
public:
	Farsight();
};

} // namespace dungeon::game::spells
