// ============================================================================
// Game/Spell/Skylight.cpp - see Skylight.h.
// ============================================================================
#include "Game/Spell/Skylight.h"

namespace dungeon::game::spells {

Skylight::Skylight()
	: LightSpell("skylight", {SpellSymbol::Air, SpellSymbol::Light}, /*power=*/8.0f,
				 /*mana=*/5.0f, /*duration=*/60.0f) {}

} // namespace dungeon::game::spells
