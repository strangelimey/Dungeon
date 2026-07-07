// ============================================================================
// Game/Spell/Push.cpp — see Push.h.
// ============================================================================
#include "Game/Spell/Push.h"

namespace dungeon::game::spells {

Push::Push()
	: BoltSpell("push", {SpellSymbol::Air, SpellSymbol::Project},
				/*power=*/4.0f, /*mana=*/6.0f, /*speed=*/12.0f, /*range=*/8.0f,
				/*push=*/1) {}

} // namespace dungeon::game::spells
