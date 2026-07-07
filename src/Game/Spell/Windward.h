// ============================================================================
// Game/Spell/Windward.h — Wind Ward (air,protect): air DEFLECTS.
//
// A bolt aimed at the warded member is turned aside outright (no strike
// roll), spending one of the ward's CHARGES (the magnitude); the last
// deflection stills the wind. Melee passes through by design. Growth
// (docs/spells.md): charge count with air power, then straying melee too.
// ============================================================================
#pragma once

#include "Game/Spell/WardSpell.h"

namespace dungeon::game::spells {

class Windward : public WardSpell {
public:
	Windward();
};

} // namespace dungeon::game::spells
