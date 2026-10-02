// ============================================================================
// Game/Spell/Airbolt.cpp — see Airbolt.h.
// ============================================================================
#include "Game/Spell/Airbolt.h"

namespace dungeon::game::spells {

Airbolt::Airbolt()
	: BoltSpell("airbolt", {SpellSymbol::Air, SpellSymbol::Project},
				/*power=*/4.0f, /*mana=*/6.0f, /*speed=*/12.0f, /*range=*/8.0f,
				/*push=*/1) {}

} // namespace dungeon::game::spells
