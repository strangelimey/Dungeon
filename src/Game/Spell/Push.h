// ============================================================================
// Game/Spell/Push.h — Push (air,project): the air school's directed
// Project-form cast, and the engine's first DISPLACEMENT effect.
//
// Air's identity: its bolt MOVES the target more than it hurts it — a struck
// survivor is shoved a cell along the bolt's travel (walls/doors/occupied
// cells stop the shove; the shove itself resolves in the world's impact
// hook). Growth (docs/spells.md): shove distance with air power; a future
// tier-3 turns it into a sweeping line/cone.
// ============================================================================
#pragma once

#include "Game/Spell/BoltSpell.h"

namespace dungeon::game::spells {

class Push : public BoltSpell {
public:
	Push();
};

} // namespace dungeon::game::spells
