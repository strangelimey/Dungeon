// ============================================================================
// Game/Spell/Scrying.cpp — see Scrying.h.
// ============================================================================
#include "Game/Spell/Scrying.h"

namespace dungeon::game::spells {

Scrying::Scrying()
	: SightSpell("scrying", {SpellSymbol::Water, SpellSymbol::Sight},
				 /*power=*/1.0f, /*mana=*/6.0f, /*duration=*/20.0f) {}

} // namespace dungeon::game::spells
