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

#include <vector>

namespace dungeon::game {

// Batched dungeon geometry, bucketed by texture variant (outer index).
struct DungeonGeometry {
	std::vector<assets::MeshData> walls;
	std::vector<assets::MeshData> floors;
	std::vector<assets::MeshData> ceilings;
};

// Instances the baked block models (assets/models/*_block.gltf) over every
// floor cell: floor + ceiling per cell and a wall block on each edge that
// borders solid rock. Each cell picks a texture variant by a stable hash.
DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 const assets::MeshData& wallBlock,
									 const assets::MeshData& floorBlock,
									 const assets::MeshData& ceilingBlock,
									 u32 wallVariants, u32 floorVariants,
									 u32 ceilingVariants);

} // namespace dungeon::game
