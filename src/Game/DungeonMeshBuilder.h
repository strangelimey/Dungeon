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

// Batched dungeon geometry, bucketed by texture variant (outer index).
struct DungeonGeometry {
	std::vector<assets::MeshData> walls;
	std::vector<assets::MeshData> floors;
	std::vector<assets::MeshData> ceilings;
};

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
