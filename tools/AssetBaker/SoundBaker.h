#pragma once

#include <string>

namespace dungeon::baker {

// Writes the synthesized sound effects as .wav files into <soundsDir>.
bool BakeSounds(const std::string& soundsDir);

} // namespace dungeon::baker
