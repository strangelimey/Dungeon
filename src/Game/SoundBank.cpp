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
	spellCast = LoadSound("spells\\cast.wav");
	spellImpact = LoadSound("spells\\impact.wav");
	spellFizzle = LoadSound("spells\\fizzle.wav");
}

} // namespace dungeon::game
