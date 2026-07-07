// ============================================================================
// Game/Spell/Splash.cpp — see Splash.h.
// ============================================================================
#include "Game/Spell/Splash.h"

namespace dungeon::game::spells {

Splash::Splash()
	: BoltSpell("splash", {SpellSymbol::Water}, /*power=*/7.0f, /*mana=*/4.0f,
				/*speed=*/7.0f, /*range=*/9.0f) {}

} // namespace dungeon::game::spells
