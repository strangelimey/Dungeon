#include "Game/DungeonMap.h"

#include "Core/Assert.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// '#' wall, '.' floor, 'T' floor with a wall torch (sconce), 'F' floor with
// a fire brazier, 'P' party start, 'S' skeleton, 'M' mummy, 'B' blob (all
// monster cells are floor), 'D' dusty floor (air turbidity 1). Every fire
// additionally raises the turbidity of its own and nearby squares — smoke
// hangs around flames even in otherwise clear halls.
const char* kLayout[] = {
	"################",
	"#P....ST#......#",
	"#.#####..#.###.#",
	"#.#....#.#.#T#.#",
	"#.#.##.#.#.#.#.#",
	"#.#.#TF#.#.#.#.#",
	"#.#.####.#.#.#.#",
	"#....B...#...#.#",
	"#.########.###.#",
	"#DDD.....#.#..F#",
	"########D#.#.###",
	"#TDDDSDDD#.#..T#",
	"#DDDDDMD#..##..#",
	"#DD#D#DT#.#..#.#",
	"#DDBDDD#..F#...#",
	"################",
};
// The lower-left block (rows 12-14) is the dust-shaft showcase: a dusty
// chamber with two freestanding columns (3,13) and (5,13), a wall torch at
// (7,13) behind them, and the mummy at (6,12) backlit beside it — the torch
// beams through the column gaps and the mummy's shadow carve visible shafts
// in the hanging dust.
} // namespace

DungeonMap::DungeonMap() {
	m_height = static_cast<int>(std::size(kLayout));
	m_width = static_cast<int>(std::string(kLayout[0]).size());
	m_cells.resize(static_cast<size_t>(m_width) * m_height, Cell::Wall);
	m_turbidity.resize(m_cells.size(), 0.0f);

	for (int z = 0; z < m_height; ++z) {
		const std::string row = kLayout[z];
		DN_ASSERT(static_cast<int>(row.size()) == m_width, "ragged map row");
		for (int x = 0; x < m_width; ++x) {
			const char c = row[static_cast<size_t>(x)];
			Cell cell = c == '#' ? Cell::Wall : Cell::Floor;
			if (c == 'D') m_turbidity[static_cast<size_t>(z) * m_width + x] = 1.0f;
			if (c == 'T') m_torches.emplace_back(x, z);
			if (c == 'F') m_braziers.emplace_back(x, z);
			if (c == 'S' || c == 'M' || c == 'B') m_monsters.push_back({c, x, z});
			if (c == 'P') {
				m_startX = x;
				m_startZ = z;
			}
			m_cells[static_cast<size_t>(z) * m_width + x] = cell;
		}
	}

	// Fires thicken the air around them (braziers more than sconces).
	for (const auto& [bx, bz] : m_braziers) AddFireTurbidity(bx, bz, 0.55f);
	for (const auto& [tx, tz] : m_torches) AddFireTurbidity(tx, tz, 0.28f);
}

// Fires raise the air turbidity of their own square and the squares nearby
// (smoke hangs around flames). Chebyshev rings: full / half / quarter.
void DungeonMap::AddFireTurbidity(int x, int z, float amount) {
	for (int dz = -2; dz <= 2; ++dz) {
		for (int dx = -2; dx <= 2; ++dx) {
			const int cx = x + dx, cz = z + dz;
			if (!IsWalkable(cx, cz)) continue;
			const int ring = std::max(std::abs(dx), std::abs(dz));
			const float weight = ring == 0 ? 1.0f : (ring == 1 ? 0.5f : 0.22f);
			float& cell = m_turbidity[static_cast<size_t>(cz) * m_width + cx];
			cell = std::min(1.0f, cell + amount * weight);
		}
	}
}

Cell DungeonMap::At(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return Cell::Wall;
	return m_cells[static_cast<size_t>(z) * m_width + x];
}

bool DungeonMap::IsWalkable(int x, int z) const { return At(x, z) == Cell::Floor; }

} // namespace dungeon::game
