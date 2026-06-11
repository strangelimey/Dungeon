#pragma once

#include "Assets/Model.h"

#include <string>

namespace dungeon::baker {

// Writes a single-mesh ModelData (optionally skinned + animated) as a .gltf
// file with an embedded base64 buffer. The game loads it back through
// assets::LoadModel.
bool WriteGltf(const assets::ModelData& model, const std::string& path);

} // namespace dungeon::baker
