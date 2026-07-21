// ============================================================================
// Game/Spell/Stonesight.h — Stone Sight (earth,sight): earth READS + REMEMBERS.
//
// Earth's peek is the reliable surveyor — the longest-lasting Sight, and it
// permanently MAPS the revealed cell (the host writes it into the fog set) so
// the layout is remembered after the peek closes. Growth (docs/spells.md):
// duration / thickness of rock it reads through with earth power.
// ============================================================================
#pragma once

#include "Game/Spell/SightSpell.h"

namespace dungeon::game::spells {

class Stonesight : public SightSpell {
public:
	Stonesight();
};

} // namespace dungeon::game::spells
