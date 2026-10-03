// ============================================================================
// Game/Spell/Stonelight.h - Stonelight (earth,light): a steady amber stone SET DOWN where it is cast
// rather than following the party; it LASTS LONGEST, MAPS what it shows and
// SHOWS the TRACKS monsters have left (Michael, lighting-updates Phase 6).
// What the school adds beyond its colour is the world's (it reads the light
// effect's school); this class is the recipe and its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/LightSpell.h"

namespace dungeon::game::spells {

class Stonelight : public LightSpell {
public:
	Stonelight();
};

} // namespace dungeon::game::spells
