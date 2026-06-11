#pragma once

#include "Assets/Model.h"
#include "Game/DungeonMap.h"

namespace dungeon::game {

struct DungeonGeometry {
    assets::MeshData walls;
    assets::MeshData floors;
    assets::MeshData ceilings;
};

// Builds world-space geometry for every floor cell: floor + ceiling quads and
// wall quads facing into the room, with normals pointing inward.
DungeonGeometry BuildDungeonGeometry(const DungeonMap& map);

} // namespace dungeon::game
