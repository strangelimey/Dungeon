// ============================================================================
// Game/Spell/Slingshot.cpp — see Slingshot.h.
// ============================================================================
#include "Game/Spell/Slingshot.h"

namespace dungeon::game::spells {

Slingshot::Slingshot()
	: BoltSpell("slingshot", {SpellSymbol::Earth, SpellSymbol::Project},
				/*power=*/18.0f, /*mana=*/10.0f, /*speed=*/9.0f,
				/*range=*/10.0f) {}

} // namespace dungeon::game::spells
