// ============================================================================
// Game/Spell/Firebolt.cpp — see Firebolt.h.
// ============================================================================
#include "Game/Spell/Firebolt.h"

namespace dungeon::game::spells {

Firebolt::Firebolt()
	: BoltSpell("firebolt", {SpellSymbol::Fire, SpellSymbol::Project},
				/*power=*/14.0f, /*mana=*/8.0f, /*speed=*/8.0f, /*range=*/8.0f) {}

} // namespace dungeon::game::spells
