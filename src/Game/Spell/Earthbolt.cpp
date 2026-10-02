// ============================================================================
// Game/Spell/Earthbolt.cpp — see Earthbolt.h.
// ============================================================================
#include "Game/Spell/Earthbolt.h"

namespace dungeon::game::spells {

Earthbolt::Earthbolt()
	: BoltSpell("earthbolt", {SpellSymbol::Earth, SpellSymbol::Project},
				/*power=*/18.0f, /*mana=*/10.0f, /*speed=*/9.0f,
				/*range=*/10.0f) {}

} // namespace dungeon::game::spells
