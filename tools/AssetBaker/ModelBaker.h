#pragma once

#include <string>

namespace dungeon::baker {

// Writes all .gltf models (dungeon blocks, pillar, monsters) into <modelsDir>.
bool BakeModels(const std::string& modelsDir);

} // namespace dungeon::baker
