// ============================================================================
// Game/Spell/Firelight.cpp - see Firelight.h.
// ============================================================================
#include "Game/Spell/Firelight.h"

namespace dungeon::game::spells {

Firelight::Firelight()
	: LightSpell("firelight", {SpellSymbol::Fire, SpellSymbol::Light}, /*power=*/8.0f,
				 /*mana=*/5.0f, /*duration=*/60.0f) {}

} // namespace dungeon::game::spells
