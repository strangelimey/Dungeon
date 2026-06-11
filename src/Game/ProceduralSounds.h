#pragma once

#include "Assets/Wav.h"

namespace dungeon::game {

// Sound effects synthesized in code so the game runs with zero asset files.
assets::SoundData MakeFootstepSound();
assets::SoundData MakeBumpSound();
assets::SoundData MakeTurnSound();
assets::SoundData MakeUiClickSound();

} // namespace dungeon::game
