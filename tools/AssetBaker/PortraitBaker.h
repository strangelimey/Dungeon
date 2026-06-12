#pragma once

#include <string>

namespace dungeon::baker {

// Renders the default party's portraits (portrait_<name>.png, 256x256) into
// <texturesDir>. Names must match the roster in src/Game/Character.cpp.
bool BakePortraits(const std::string& texturesDir);

} // namespace dungeon::baker
