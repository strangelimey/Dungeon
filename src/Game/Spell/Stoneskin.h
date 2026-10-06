// ============================================================================
// Game/Spell/Stoneskin.h — Stone Skin (earth,protect): earth HARDENS.
//
// The caster's skin turns to stone: PHYSICAL resist for the duration - the
// ward's magnitude x balance.cat `stoneskin_resist`, added at the mitigate
// stage (fx::StoneskinEffect::ResistFor); elemental damage passes it by.
// Growth (docs/spells.md): the magnitude is the skill-scaled cast power, and
// with Ingwaz the ward lands on every standing member (ModifiedSpell).
// ============================================================================
#pragma once

#include "Game/Spell/WardSpell.h"

namespace dungeon::game::spells {

class Stoneskin : public WardSpell {
public:
	Stoneskin();
};

} // namespace dungeon::game::spells
