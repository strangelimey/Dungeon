// ============================================================================
// Game/Spell/Slingshot.h — Slingshot (earth,project): the earth school's
// directed Project-form cast.
//
// Day one: the hardest, and a fast, bolt. Growth (docs/spells.md): projectile
// size/weight scale with earth power (gravel → stone → boulder), inheriting
// Pebble's growth line.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Slingshot : public BoltSpell {
public:
	Slingshot();
};

} // namespace dungeon::game::spells
