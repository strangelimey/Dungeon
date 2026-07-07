// ============================================================================
// Game/Spell/Waterbolt.cpp — see Waterbolt.h.
// ============================================================================
#include "Game/Spell/Waterbolt.h"

namespace dungeon::game::spells {

Waterbolt::Waterbolt()
	: BoltSpell("waterbolt", {SpellSymbol::Water, SpellSymbol::Project},
				/*power=*/12.0f, /*mana=*/7.0f, /*speed=*/9.0f,
				/*range=*/10.0f) {}

} // namespace dungeon::game::spells
