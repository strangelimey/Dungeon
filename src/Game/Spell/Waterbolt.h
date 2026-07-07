// ============================================================================
// Game/Spell/Waterbolt.h — Water Bolt (water,project): the water school's
// directed Project-form cast.
//
// Day one: a fast middleweight bolt. Growth (docs/spells.md): will douse
// fires — that behaviour lands as a Cast()/impact override here once fire
// ignition exists.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Waterbolt : public BoltSpell {
public:
	Waterbolt();
};

} // namespace dungeon::game::spells
