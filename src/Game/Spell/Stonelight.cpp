// ============================================================================
// Game/Spell/Stonelight.cpp - see Stonelight.h.
// ============================================================================
#include "Game/Spell/Stonelight.h"

namespace dungeon::game::spells {

Stonelight::Stonelight()
	: LightSpell("stonelight", {SpellSymbol::Earth, SpellSymbol::Light}, /*power=*/8.0f,
				 /*mana=*/5.0f, /*duration=*/150.0f) {}

} // namespace dungeon::game::spells
