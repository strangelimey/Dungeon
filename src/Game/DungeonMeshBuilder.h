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
