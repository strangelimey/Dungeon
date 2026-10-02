// ============================================================================
// Game/Spell/Firebolt.h - Fire Bolt (Kenaz Tiwaz): the fire school's Project
// form, a bolt of flame that strikes ONE target and does not explode
// (spell-updates, Michael); with Hagalaz after it, it does (ModifiedSpell).
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Firebolt : public BoltSpell {
public:
	Firebolt();
};

} // namespace dungeon::game::spells
