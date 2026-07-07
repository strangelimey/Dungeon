// ============================================================================
// Game/Spell/Rock.h — Pebble (earth): the earth school's tier-1 bolt.
//
// Day one: a slow, solid stone. Growth (docs/spells.md): projectile size and
// damage scale with earth power — gravel toward a real stone.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Rock : public BoltSpell {
public:
	Rock();
};

} // namespace dungeon::game::spells
