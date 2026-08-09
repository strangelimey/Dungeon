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

	// Imported (gitignored, tools\FetchSounds.ps1). LoadSound warns and leaves
	// these empty when they are absent, and Play is a no-op on an empty sound —
	// so a worktree with no audio provisioned runs quiet rather than failing.
	ambDungeon = LoadSound("amb_dungeon.wav");
	fireLoop = LoadSound("fire_loop.wav");
}

} // namespace dungeon::game
