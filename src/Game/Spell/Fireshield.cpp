// ============================================================================
// Game/Spell/Fireshield.cpp — see Fireshield.h.
// ============================================================================
#include "Game/Spell/Fireshield.h"

namespace dungeon::game::spells {

Fireshield::Fireshield()
	: WardSpell("fireshield", {SpellSymbol::Fire, SpellSymbol::Protect},
				/*power=*/6.0f, /*mana=*/8.0f, /*duration=*/30.0f) {}

} // namespace dungeon::game::spells
