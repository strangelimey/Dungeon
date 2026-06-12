#pragma once

#include <string>

namespace dungeon::baker {

// Imports a downloaded PBR texture set (Poly Haven, ambientCG, Megascans...)
// into the engine's packed format:
//   <texturesDir>/<name>.png    albedo (AO multiplied in if present)
//   <texturesDir>/<name>_n.png  tangent-space normal (RGB) + height (A)
// Maps are discovered by filename convention inside <sourceDir>; OpenGL-style
// normals (green up) are flipped to the DirectX convention automatically when
// the filename says so, or always when forceFlipGreen is set.
bool ImportPbrTextureSet(const std::string& sourceDir, const std::string& texturesDir,
						 const std::string& outputName, bool forceFlipGreen);

} // namespace dungeon::baker
