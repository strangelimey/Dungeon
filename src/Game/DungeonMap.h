// ============================================================================
// Game/DungeonMap.h — the level definition, loaded from a data file.
//
// Levels are ASCII grid text files under assets/maps/ (see level1.map for
// the glyph legend: '#' rock, '.' floor, 'D' dust, 'T' sconce, 'F' brazier,
// 'P' start, 'S'/'M'/'B' monsters; ';' lines are comments). The constructor
// parses and validates the file — unknown glyphs, ragged rows, or a missing
// start cell fail hard with the file name and position.
//
// Cell (x, z) maps to world space as center ((x+0.5), 0, (z+0.5)) * kCellSize
// with +X = east and +Z = south (row index grows southward). The geometry
// builder, fires, turbidity grid, and monster spawns all derive from this.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <string>
#include <vector>

namespace dungeon::game {

// World-space dimensions shared by geometry, lighting, and the camera.
inline constexpr float kCellSize = 2.0f;   // square cells, 2m on a side
inline constexpr float kWallHeight = 2.5f; // floor to ceiling
inline constexpr float kEyeHeight = 1.55f; // camera height above the floor

enum class Cell : u8 { Wall, Floor };

// Grid-based dungeon. Coordinates: x = column, z = row; world position of a
// cell center is ((x + 0.5) * kCellSize, 0, (z + 0.5) * kCellSize).
class DungeonMap {
public:
	// Loads and validates a .map file; failures are fatal with a clear message.
	explicit DungeonMap(const std::string& path);

	int Width() const { return m_width; }
	int Height() const { return m_height; }

	bool IsWalkable(int x, int z) const;
	Cell At(int x, int z) const;

	// Air turbidity 0 (clear) .. 1 (thick dust) for a cell; walls return 0.
	float Turbidity(int x, int z) const {
		if (x < 0 || z < 0 || x >= m_width || z >= m_height) return 0.0f;
		return m_turbidity[static_cast<size_t>(z) * m_width + x];
	}

	Vec3 CellCenter(int x, int z, float y = 0.0f) const {
		return {(static_cast<float>(x) + 0.5f) * kCellSize, y,
				(static_cast<float>(z) + 0.5f) * kCellSize};
	}

	int StartX() const { return m_startX; }
	int StartZ() const { return m_startZ; }
	const std::vector<std::pair<int, int>>& TorchCells() const { return m_torches; }
	const std::vector<std::pair<int, int>>& BrazierCells() const { return m_braziers; }

	// Monster spawn points: kind is 'S' skeleton, 'M' mummy, 'B' blob.
	struct MonsterSpawn {
		char kind;
		int x, z;
	};
	const std::vector<MonsterSpawn>& MonsterSpawns() const { return m_monsters; }

private:
	void AddFireTurbidity(int x, int z, float amount);

	int m_width = 0;
	int m_height = 0;
	int m_startX = 1;
	int m_startZ = 1;
	std::vector<Cell> m_cells;
	std::vector<float> m_turbidity; // parallel to m_cells
	std::vector<std::pair<int, int>> m_torches;
	std::vector<std::pair<int, int>> m_braziers;
	std::vector<MonsterSpawn> m_monsters;
};

} // namespace dungeon::game
