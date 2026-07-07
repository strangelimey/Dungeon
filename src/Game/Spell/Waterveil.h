// ============================================================================
// Game/Spell/Waterveil.h — Water Veil (water,protect): water ABSORBS.
//
// A flowing film soaks damage into a POOL (the ward's magnitude) before any
// reaches health — every source alike, melee, ranged, even a wall bump — and
// BURSTS when the pool is spent. Growth (docs/spells.md): pool size with
// water power; quenching fire damage entirely once schools flavour damage.
// ============================================================================
#pragma once

#include "Game/Spell/WardSpell.h"

namespace dungeon::game::spells {

class Waterveil : public WardSpell {
public:
	Waterveil();
};

} // namespace dungeon::game::spells
