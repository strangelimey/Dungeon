#include "Game/DungeonMap.h"

#include "Core/Assert.h"

namespace dungeon::game {

namespace {
// '#' wall, '.' floor, 'T' floor with a wall torch, 'P' party start,
// 'S' skeleton, 'M' mummy, 'B' blob (all monster cells are floor).
const char* kLayout[] = {
	"################",
	"#P....ST#......#",
	"#.######.#.###.#",
	"#.#....#.#.#T#.#",
	"#.#.##.#.#.#.#.#",
	"#.#.#T.#.#.#.#.#",
	"#.#.####.#.#.#.#",
	"#.#..B...#...#.#",
	"#.########.###.#",
	"#...M....#.#...#",
	"########.#.#.###",
	"#T...S...#.#..T#",
	"#.#######..##..#",
	"#.#....T#.#..#.#",
	"#..B...#...#...#",
	"################",
};
} // namespace

DungeonMap::DungeonMap() {
	m_height = static_cast<int>(std::size(kLayout));
	m_width = static_cast<int>(std::string(kLayout[0]).size());
	m_cells.resize(static_cast<size_t>(m_width) * m_height, Cell::Wall);

	for (int z = 0; z < m_height; ++z) {
		const std::string row = kLayout[z];
		DN_ASSERT(static_cast<int>(row.size()) == m_width, "ragged map row");
		for (int x = 0; x < m_width; ++x) {
			const char c = row[static_cast<size_t>(x)];
			Cell cell = c == '#' ? Cell::Wall : Cell::Floor;
			if (c == 'T') m_torches.emplace_back(x, z);
			if (c == 'S' || c == 'M' || c == 'B') m_monsters.push_back({c, x, z});
			if (c == 'P') {
				m_startX = x;
				m_startZ = z;
			}
			m_cells[static_cast<size_t>(z) * m_width + x] = cell;
		}
	}
}

Cell DungeonMap::At(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return Cell::Wall;
	return m_cells[static_cast<size_t>(z) * m_width + x];
}

bool DungeonMap::IsWalkable(int x, int z) const { return At(x, z) == Cell::Floor; }

} // namespace dungeon::game
