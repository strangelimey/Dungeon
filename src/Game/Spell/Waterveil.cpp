// ============================================================================
// Game/Spell/Waterveil.cpp — see Waterveil.h.
// ============================================================================
#include "Game/Spell/Waterveil.h"

namespace dungeon::game::spells {

Waterveil::Waterveil()
	: WardSpell("waterveil", {SpellSymbol::Water, SpellSymbol::Protect},
				/*power=*/20.0f, /*mana=*/8.0f, /*duration=*/60.0f) {}

} // namespace dungeon::game::spells
