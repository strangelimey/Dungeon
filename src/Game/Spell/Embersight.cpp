// ============================================================================
// Game/Spell/Embersight.cpp — see Embersight.h.
// ============================================================================
#include "Game/Spell/Embersight.h"

namespace dungeon::game::spells {

Embersight::Embersight()
	: SightSpell("embersight", {SpellSymbol::Fire, SpellSymbol::Sight},
				 /*power=*/8.0f, /*mana=*/6.0f, /*duration=*/20.0f) {}

} // namespace dungeon::game::spells
