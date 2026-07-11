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
//
// `textureSet` reuses an already-imported set instead of packing this folder's
// maps: when non-empty the PBR import/mip step is skipped entirely and the model
// is written referencing that set name (a flat white material, like the normal
// path — the game binds the set by name, not via the glTF material). This lets
// every item split out of one multi-mesh fab pack share a single material set
// imported once, rather than re-baking the same maps per item.
//
// Wall fixtures (sconces) place differently from floor props: the game renders
// them at y=0 against the mount wall, so the MESH carries its own hanging
// height and wall contact (the procedural sconce is authored at y 1.27..1.79
// against z=0, arm reaching +Z). `liftMeters` raises the grounded mesh to that
// hanging height, and `wallAlign` puts its back face AT z=0 (min z = 0, room
// side +Z) instead of centering Z — use --yaw first to spin the back to -Z.
bool ImportModel(const std::string& sourcePath, const std::string& assetsDir,
				 const std::string& name, float targetHeight, float yawDegrees,
				 char upAxis, const std::string& textureSet = {},
				 float liftMeters = 0.0f, bool wallAlign = false);

} // namespace dungeon::baker
