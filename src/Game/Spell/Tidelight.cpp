// ============================================================================
// Game/Spell/Tidelight.cpp - see Tidelight.h.
// ============================================================================
#include "Game/Spell/Tidelight.h"

namespace dungeon::game::spells {

Tidelight::Tidelight()
	: LightSpell("tidelight", {SpellSymbol::Water, SpellSymbol::Light}, /*power=*/8.0f,
				 /*mana=*/5.0f, /*duration=*/60.0f) {}

} // namespace dungeon::game::spells
