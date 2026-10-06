#pragma once

#include <optional>
#include <string>

namespace dungeon::baker {

// Imports a downloaded PBR texture set (Poly Haven, ambientCG, Megascans...)
// into the engine's packed format:
//   <texturesDir>/<name>.png    albedo (AO multiplied in if present)
//   <texturesDir>/<name>_n.png  tangent-space normal (RGB) + height (A)
// Maps are discovered by filename convention inside <sourceDir>. OpenGL-style
// normals (green up) are flipped to the DirectX convention as `flipGreen` says:
// true flips, false does not, and nullopt goes by the normal map's name
// (Assets/PbrMaps.h NormalNameLooksGl). The editor always says one or the other
// (code-review C393: it could only ever force the flip ON, so unticking a wrong
// guess changed nothing).
bool ImportPbrTextureSet(const std::string& sourceDir, const std::string& texturesDir,
						 const std::string& outputName, std::optional<bool> flipGreen);

} // namespace dungeon::baker
