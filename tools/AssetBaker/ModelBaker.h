#pragma once

#include <string>

namespace dungeon::baker {

// Writes all .gltf models (dungeon blocks, pillar, monsters) into <modelsDir>.
// The worn block variants displace by the scanned height maps found in
// <texturesDir> (<texture>_1k_n.png alpha), falling back to procedural wear
// with a warning when a set is not installed.
bool BakeModels(const std::string& modelsDir, const std::string& texturesDir);

// Bakes the worn block meshes (3 tiers) for a single surface texture set —
// `kind` is "wall", "floor", or "ceiling". Used after importing a new texture
// set so a level can reference it. `assetsDir` holds models/ and textures/.
bool BakeWornBlocks(const std::string& kind, const std::string& name,
					const std::string& assetsDir);

} // namespace dungeon::baker
