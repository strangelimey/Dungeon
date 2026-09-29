// ============================================================================
// Game/SoundBank.h — the game's loaded sound effects.
//
// One struct so every system that plays feedback (party movement, monsters,
// UI clicks) shares the same data. Loaded as a boot task before the landing
// page; missing files warn and run silent (see AssetUtil's LoadSound). The
// AudioEngine reads the buffers in place, so the bank must outlive playback —
// it lives in Game for the app's lifetime.
// ============================================================================
#pragma once

#include "Assets/Wav.h"

#include <array>

namespace dungeon::game {

struct SoundBank {
	assets::SoundData footstep;
	assets::SoundData bump;
	assets::SoundData turn;
	assets::SoundData click;
	assets::SoundData monster;
	assets::SoundData oof; // party grunt when a blocked move jars them

	// Spell effects (assets/sounds/spells/).
	assets::SoundData spellCast;   // a spell is released
	assets::SoundData spellImpact; // a bolt strikes a monster
	assets::SoundData spellFizzle; // no recipe / no mana / hits a wall

	void Load();
	// Every sound in the bank, for walks over all of them (the voice reserve).
	std::array<const assets::SoundData*, 9> All() const {
		return {&footstep, &bump, &turn, &click, &monster, &oof,
				&spellCast, &spellImpact, &spellFizzle};
	}
};

} // namespace dungeon::game
