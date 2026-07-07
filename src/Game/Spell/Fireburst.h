// ============================================================================
// Game/Spell/Fireburst.h — Fire Burst (fire,project): the fire school's
// directed Project-form cast.
//
// Day one: a strong fire bolt. Growth (docs/spells.md): with fire power it
// becomes a sustained FLAMETHROWER — a held jet rather than a single bolt.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Fireburst : public BoltSpell {
public:
	Fireburst();
};

} // namespace dungeon::game::spells
