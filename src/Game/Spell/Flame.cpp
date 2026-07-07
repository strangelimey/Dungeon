// ============================================================================
// Game/Spell/Flame.cpp — see Flame.h.
// ============================================================================
#include "Game/Spell/Flame.h"

namespace dungeon::game::spells {

Flame::Flame()
	: BoltSpell("flame", {SpellSymbol::Fire}, /*power=*/8.0f, /*mana=*/4.0f,
				/*speed=*/7.0f, /*range=*/8.0f) {}

} // namespace dungeon::game::spells
