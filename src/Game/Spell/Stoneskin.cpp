// ============================================================================
// Game/Spell/Stoneskin.cpp — see Stoneskin.h.
// ============================================================================
#include "Game/Spell/Stoneskin.h"

namespace dungeon::game::spells {

Stoneskin::Stoneskin()
	: WardSpell("stoneskin", {SpellSymbol::Earth, SpellSymbol::Protect},
				/*power=*/6.0f, /*mana=*/8.0f, /*duration=*/30.0f) {}

} // namespace dungeon::game::spells
