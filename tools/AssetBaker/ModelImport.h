#pragma once

#include <string>

namespace dungeon::baker {

// Imports a raw authored model (.gltf/.glb/.obj, or a folder containing one)
// into the engine's conventions and writes assets/models/<name>.gltf:
//   * merges every mesh/primitive into one (baking node transforms), since
//     WriteGltf takes a single static mesh
//   * normalizes orientation (optional Z-up -> Y-up and a yaw) and scale, then
//     grounds the model (min y = 0) and centers it in XZ, so it drops onto a
//     cell floor facing +Z like the procedural props
//   * if the source folder carries PBR maps, imports them as the texture set
//     <name> (albedo/normal+height/ORM) so the game binds them by model name
//
// `targetHeight` <= 0 auto-fits the largest extent to roughly one cell; `upAxis`
// is 'y' (default) or 'z'. Authored meshes are consistently wound, so the game
// renders them back-face culled.
bool ImportModel(const std::string& sourcePath, const std::string& assetsDir,
				 const std::string& name, float targetHeight, float yawDegrees,
				 char upAxis);

} // namespace dungeon::baker
