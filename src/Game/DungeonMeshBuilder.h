// ============================================================================
// Game/DungeonMeshBuilder.h — bakes the map into batched render geometry.
//
// Runs once at load. For every floor cell it stamps copies of the block
// models (wall/floor/ceiling .gltf) into big combined vertex buffers — CPU
// "instancing" that turns ~700 block placements into at most
// (#texture variants) draw calls per surface type per frame. Each cell picks
// its texture variant with a position hash, so the mix of brick/stone/mossy
// walls is varied but identical on every run.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Game/DungeonMap.h"

#include <span>
#include <vector>

namespace dungeon::game {

// One spatial chunk of batched geometry: the cells of a fixed map region that
// share a texture variant, combined into one mesh with its world AABB. Chunking
// (instead of one level-wide mesh per variant) lets the renderer frustum-cull
// off-screen regions in the main pass and sphere-cull out-of-range regions per
// shadow cube, at the cost of a few more draw calls.
struct GeometryChunk {
	int variant = 0; // index into the surface's parallel texture arrays
	assets::MeshData mesh;
	Vec3 boundsMin{}, boundsMax{};
};

// Batched dungeon geometry as cullable chunks (variant carried per chunk).
struct DungeonGeometry {
	std::vector<GeometryChunk> walls;
	std::vector<GeometryChunk> floors;
	std::vector<GeometryChunk> ceilings;
};

// Map cells per chunk edge (chunk = kChunkCells x kChunkCells cells). Sized so
// a chunk (~9.6 m at kCellSize) is a bit larger than a light radius — small
// enough that a shadow cube touches only a handful of chunks.
inline constexpr int kChunkCells = 4;

// Instances the baked block models over every floor cell: floor + ceiling
// per cell and a wall block on each edge that borders solid rock. Each block
// span holds one mesh per texture variant; a cell picks its variant by a
// stable hash and stamps the MATCHING mesh into that variant's bucket, so
// geometric relief always pairs with the texture drawn over it.
DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 std::span<const assets::MeshData> wallBlocks,
									 std::span<const assets::MeshData> floorBlocks,
									 std::span<const assets::MeshData> ceilingBlocks);

} // namespace dungeon::game
