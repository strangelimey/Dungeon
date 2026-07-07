// ============================================================================
// Game/Spell/Fireburst.cpp — see Fireburst.h.
// ============================================================================
#include "Game/Spell/Fireburst.h"

namespace dungeon::game::spells {

Fireburst::Fireburst()
	: BoltSpell("fireburst", {SpellSymbol::Fire, SpellSymbol::Project},
				/*power=*/14.0f, /*mana=*/8.0f, /*speed=*/8.0f, /*range=*/8.0f) {}

} // namespace dungeon::game::spells
