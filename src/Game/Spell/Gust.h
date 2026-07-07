// ============================================================================
// Game/Spell/Gust.h — Puff of Wind (air): the air school's tier-1 bolt.
//
// Day one: a fast, light bolt. Growth (docs/spells.md): becomes the monster-
// pushing gust — air's identity is displacement, barely damage.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Gust : public BoltSpell {
public:
	Gust();
};

} // namespace dungeon::game::spells
