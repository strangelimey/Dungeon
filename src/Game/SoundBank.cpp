// ============================================================================
// Game/SoundBank.cpp — see SoundBank.h.
// ============================================================================
#include "Game/SoundBank.h"

#include "Game/AssetUtil.h"

namespace dungeon::game {

void SoundBank::Load() {
	footstep = LoadSound("footstep.wav");
	bump = LoadSound("bump.wav");
	turn = LoadSound("turn.wav");
	click = LoadSound("click.wav");
	monster = LoadSound("monster.wav");
	oof = LoadSound("oof.wav");
}

} // namespace dungeon::game
