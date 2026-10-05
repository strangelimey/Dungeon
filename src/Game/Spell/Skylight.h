// ============================================================================
// Game/Spell/Skylight.h - Skylight (air,light): a pale storm-flicker, the widest reach and the
// dimmest; it CRACKLES at foes in its reach and WARNS - its flicker quickens
// while a monster near has noticed the party (Michael, lighting-updates
// Phase 6).
// What the school adds beyond its colour is the world's (it reads the light
// effect's school); this class is the recipe and its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/LightSpell.h"

namespace dungeon::game::spells {

class Skylight : public LightSpell {
public:
	Skylight();
};

} // namespace dungeon::game::spells
