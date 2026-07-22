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
// SIZES ARE IN UNITS, not metres: 1.0 = one dungeon square (game::kUnit in
// src/Game/DungeonMap.h). Every model on disk is unit-space and the engine
// scales by kUnit when it places one, so a bought prop keeps its proportion of
// a square whatever a square measures — see docs/authoring-scale.md.
// `targetHeight` <= 0 auto-fits the largest extent to 0.8 of a square; `upAxis`
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
// height and wall contact (the procedural sconce is authored at y 0.51..0.72
// units against z=0, arm reaching +Z). `liftUnits` raises the grounded mesh to
// that hanging height, and `wallAlign` puts its back face AT z=0 (min z = 0, room
// side +Z) instead of centering Z — use --yaw first to spin the back to -Z.
// `rawTransform` trusts the source's placement entirely — no orient, scale,
// ground, center, lift, or wall shift. For pieces ConvertMesh --split-whole
// already normalized as one scene (a bowl + the coals nested in it): each
// piece re-fit on its own bounds would break their mutual alignment.
bool ImportModel(const std::string& sourcePath, const std::string& assetsDir,
				 const std::string& name, float targetHeight, float yawDegrees,
				 char upAxis, const std::string& textureSet = {},
				 float liftUnits = 0.0f, bool wallAlign = false,
				 bool rawTransform = false);

// Uniformly rescales an already-imported model in place (assets/models/<name>
// .gltf), rewriting it through the same writer. The fix-up for a model that
// landed at the wrong size — chiefly the one-time metres-to-units migration
// (factor 1/2.5) for props imported before the unit convention existed; see
// docs/authoring-scale.md.
//
// Only safe for what ImportModel emits: ONE mesh, no skeleton/clips, no
// embedded images. A rigged or multi-material model (.glb with embedded
// textures) would lose data in the round-trip, so those are refused — re-run
// their import pipeline instead (tools/FetchModels.ps1, FetchAnimLibrary.ps1).
bool RescaleModel(const std::string& assetsDir, const std::string& name,
				  float factor);

} // namespace dungeon::baker
