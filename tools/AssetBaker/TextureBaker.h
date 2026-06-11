#pragma once

#include <string>

namespace dungeon::baker {

// Writes all wall/floor/ceiling texture variants (albedo + "_n" normal+height
// maps) into <texturesDir>.
bool BakeTextures(const std::string& texturesDir);

} // namespace dungeon::baker
