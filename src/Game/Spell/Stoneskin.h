// ============================================================================
// Game/Spell/Stoneskin.h — Stone Skin (earth,protect): earth HARDENS.
//
// The caster's skin turns to stone: a flat armor bonus (the ward's magnitude
// rides Character::Armor()) for the duration. Growth (docs/spells.md): armor
// with earth power; the tier-3 outward form is a stone WALL on a map cell.
// ============================================================================
#pragma once

#include "Game/Spell/WardSpell.h"

namespace dungeon::game::spells {

class Stoneskin : public WardSpell {
public:
	Stoneskin();
};

} // namespace dungeon::game::spells
