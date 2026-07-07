// ============================================================================
// Game/Spell/Rock.cpp — see Rock.h.
// ============================================================================
#include "Game/Spell/Rock.h"

namespace dungeon::game::spells {

Rock::Rock()
	: BoltSpell("rock", {SpellSymbol::Earth}, /*power=*/12.0f, /*mana=*/6.0f,
				/*speed=*/5.0f, /*range=*/7.0f) {}

} // namespace dungeon::game::spells
