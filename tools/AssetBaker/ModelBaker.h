#pragma once

#include <string>

namespace dungeon::baker {

// Writes all .gltf models (dungeon blocks, pillar, monsters) into <modelsDir>.
// The worn block variants displace by the scanned height maps found in
// <texturesDir> (<texture>_1k_n.png alpha), falling back to procedural wear
// with a warning when a set is not installed.
bool BakeModels(const std::string& modelsDir, const std::string& texturesDir);

} // namespace dungeon::baker
