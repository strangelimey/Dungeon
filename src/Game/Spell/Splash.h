// ============================================================================
// Game/Spell/Splash.h — Splash (water): the water school's tier-1 bolt.
//
// Day one: a middleweight bolt. Growth (docs/spells.md): fills an empty vial
// held in the OTHER hand (potions), then the deluge damage form — the vial
// behaviour lands as a Cast() override here once container items exist.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Splash : public BoltSpell {
public:
	Splash();
};

} // namespace dungeon::game::spells
