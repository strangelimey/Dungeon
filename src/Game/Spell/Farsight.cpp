// ============================================================================
// Game/Spell/Farsight.cpp — see Farsight.h.
// ============================================================================
#include "Game/Spell/Farsight.h"

namespace dungeon::game::spells {

Farsight::Farsight()
	: SightSpell("farsight", {SpellSymbol::Air, SpellSymbol::Sight},
				 /*power=*/3.0f, /*mana=*/6.0f, /*duration=*/20.0f) {}

} // namespace dungeon::game::spells
