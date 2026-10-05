// ============================================================================
// Game/Spell/Firelight.h - Firelight (fire,light): the brightest, a torch's warm flicker that casts
// SHADOWS; it KINDLES the unlit sconces and braziers the party passes and
// SCORCHES what comes close (Michael, lighting-updates Phase 6).
// What the school adds beyond its colour is the world's (it reads the light
// effect's school); this class is the recipe and its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/LightSpell.h"

namespace dungeon::game::spells {

class Firelight : public LightSpell {
public:
	Firelight();
};

} // namespace dungeon::game::spells
