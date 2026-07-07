// ============================================================================
// Game/Spell/Windward.cpp — see Windward.h.
// ============================================================================
#include "Game/Spell/Windward.h"

namespace dungeon::game::spells {

Windward::Windward()
	: WardSpell("windward", {SpellSymbol::Air, SpellSymbol::Protect},
				/*power=*/3.0f, /*mana=*/8.0f, /*duration=*/60.0f) {}

} // namespace dungeon::game::spells
