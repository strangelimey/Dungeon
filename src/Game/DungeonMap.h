#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <string>
#include <vector>

namespace dungeon::game {

inline constexpr float kCellSize = 2.0f;
inline constexpr float kWallHeight = 2.5f;
inline constexpr float kEyeHeight = 1.55f;

enum class Cell : u8 { Wall, Floor };

// Grid-based dungeon. Coordinates: x = column, z = row; world position of a
// cell center is ((x + 0.5) * kCellSize, 0, (z + 0.5) * kCellSize).
class DungeonMap {
public:
    DungeonMap();

    int Width() const { return m_width; }
    int Height() const { return m_height; }

    bool IsWalkable(int x, int z) const;
    Cell At(int x, int z) const;

    Vec3 CellCenter(int x, int z, float y = 0.0f) const {
        return {(static_cast<float>(x) + 0.5f) * kCellSize, y,
                (static_cast<float>(z) + 0.5f) * kCellSize};
    }

    int StartX() const { return m_startX; }
    int StartZ() const { return m_startZ; }
    const std::vector<std::pair<int, int>>& TorchCells() const { return m_torches; }

private:
    int m_width = 0;
    int m_height = 0;
    int m_startX = 1;
    int m_startZ = 1;
    std::vector<Cell> m_cells;
    std::vector<std::pair<int, int>> m_torches;
};

} // namespace dungeon::game
