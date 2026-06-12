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

namespace dungeon::game {

struct SoundBank {
	assets::SoundData footstep;
	assets::SoundData bump;
	assets::SoundData turn;
	assets::SoundData click;
	assets::SoundData monster;

	void Load();
};

} // namespace dungeon::game
