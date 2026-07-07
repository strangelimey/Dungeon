// ============================================================================
// Game/Spell/Gust.cpp — see Gust.h.
// ============================================================================
#include "Game/Spell/Gust.h"

namespace dungeon::game::spells {

Gust::Gust()
	: BoltSpell("gust", {SpellSymbol::Air}, /*power=*/5.0f, /*mana=*/3.0f,
				/*speed=*/11.0f, /*range=*/11.0f) {}

} // namespace dungeon::game::spells
