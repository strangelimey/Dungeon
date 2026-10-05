// ============================================================================
// Game/Spell/Tidelight.h - Tidelight (water,light): a cool shimmering blue that CLEARS THE HAZE
// round the party, SOOTHES (stamina comes back faster in it) and QUENCHES
// fire on the party (Michael, lighting-updates Phase 6).
// What the school adds beyond its colour is the world's (it reads the light
// effect's school); this class is the recipe and its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/LightSpell.h"

namespace dungeon::game::spells {

class Tidelight : public LightSpell {
public:
	Tidelight();
};

} // namespace dungeon::game::spells
