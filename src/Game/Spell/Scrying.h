// ============================================================================
// Game/Spell/Scrying.h — Scrying (water,sight): water reveals the HIDDEN.
//
// Water's peek is the scryer's mirror — through the ghosted wall it reveals
// what lies CONCEALED in the cell beyond (secret doors, hidden buttons, trap
// pits when those exist). Until hidden content lands it reads as the clearest
// plain peek. Growth (docs/spells.md): tier of hidden thing revealed with
// water power.
// ============================================================================
#pragma once

#include "Game/Spell/SightSpell.h"

namespace dungeon::game::spells {

class Scrying : public SightSpell {
public:
	Scrying();
};

} // namespace dungeon::game::spells
