// ============================================================================
// Game/Spell/Stonesight.cpp — see Stonesight.h.
// ============================================================================
#include "Game/Spell/Stonesight.h"

namespace dungeon::game::spells {

Stonesight::Stonesight()
	: SightSpell("stonesight", {SpellSymbol::Earth, SpellSymbol::Sight},
				 /*power=*/1.0f, /*mana=*/6.0f, /*duration=*/30.0f) {}

} // namespace dungeon::game::spells
