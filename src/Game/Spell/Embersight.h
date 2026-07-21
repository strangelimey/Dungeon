// ============================================================================
// Game/Spell/Embersight.h — Ember Sight (fire,sight): fire sees LIFE.
//
// Fire's peek lights the cell beyond the ghosted wall and glows any living
// creature there with its heat — see monsters in the dark past the stone. The
// lighting is host-side (DungeonWorld reads the active fire Sight and lights
// the revealed cell); this class is the fire-flavoured recipe. Growth
// (docs/spells.md): reach/brightness with fire power.
// ============================================================================
#pragma once

#include "Game/Spell/SightSpell.h"

namespace dungeon::game::spells {

class Embersight : public SightSpell {
public:
	Embersight();
};

} // namespace dungeon::game::spells
