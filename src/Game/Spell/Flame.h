// ============================================================================
// Game/Spell/Flame.h — Puff of Flame (fire): the fire school's tier-1 bolt.
//
// Day one: a small fire bolt. Growth (docs/spells.md): lights sconces and
// torches, a brief light flash on cast, then the fire-blast damage form —
// that behaviour lands as a Cast() override here (base bolt + ignition).
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Flame : public BoltSpell {
public:
	Flame();
};

} // namespace dungeon::game::spells
