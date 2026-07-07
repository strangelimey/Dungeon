// ============================================================================
// Game/Spell/Fireshield.h — Fire Shield (fire,protect): fire RETALIATES.
//
// Flames wreathe the bearer: a monster that LANDS a melee blow is scorched
// for the ward's magnitude (the hit itself is not reduced — earth is the
// school that hardens). Growth (docs/spells.md): retaliation damage with
// fire power; igniting flavour later.
// ============================================================================
#pragma once

#include "Game/Spell/WardSpell.h"

namespace dungeon::game::spells {

class Fireshield : public WardSpell {
public:
	Fireshield();
};

} // namespace dungeon::game::spells
