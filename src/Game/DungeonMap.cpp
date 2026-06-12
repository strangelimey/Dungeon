#include "Game/DungeonMap.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {

// Splits the raw map file into grid rows: strips '\r', drops blank lines and
// ';' comment lines. The grid is everything that remains, in order.
std::vector<std::string> ReadGridRows(const std::vector<u8>& bytes) {
	std::vector<std::string> rows;
	std::string line;
	auto flush = [&] {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty() && line[0] != ';') rows.push_back(line);
		line.clear();
	};
	for (const u8 byte : bytes) {
		if (static_cast<char>(byte) == '\n') flush();
		else line.push_back(static_cast<char>(byte));
	}
	flush();
	return rows;
}

} // namespace

DungeonMap::DungeonMap(const std::string& path) {
	auto bytes = assets::ReadBinaryFile(path);
	DN_ASSERT(bytes.has_value(), bytes.error());
	const std::vector<std::string> rows = ReadGridRows(*bytes);
	DN_ASSERT(!rows.empty(), "map file has no grid rows: " + path);

	m_height = static_cast<int>(rows.size());
	m_width = static_cast<int>(rows[0].size());
	m_cells.resize(static_cast<size_t>(m_width) * m_height, Cell::Wall);
	m_turbidity.resize(m_cells.size(), 0.0f);

	bool foundStart = false;
	for (int z = 0; z < m_height; ++z) {
		const std::string& row = rows[static_cast<size_t>(z)];
		DN_ASSERT(static_cast<int>(row.size()) == m_width,
				  std::format("ragged map row {} in {}", z, path));
		for (int x = 0; x < m_width; ++x) {
			const char c = row[static_cast<size_t>(x)];
			switch (c) {
			case '#': break; // solid rock (the default)
			case '.': break;
			case 'D': m_turbidity[static_cast<size_t>(z) * m_width + x] = 1.0f; break;
			case 'T': m_torches.emplace_back(x, z); break;
			case 'F': m_braziers.emplace_back(x, z); break;
			case 'S':
			case 'M':
			case 'B': m_monsters.push_back({c, x, z}); break;
			case 'P':
				DN_ASSERT(!foundStart,
						  std::format("multiple 'P' start cells in {}", path));
				m_startX = x;
				m_startZ = z;
				foundStart = true;
				break;
			default:
				DN_ASSERT(false, std::format("unknown map glyph '{}' at column {}, row {} in {}",
											 c, x, z, path));
			}
			m_cells[static_cast<size_t>(z) * m_width + x] =
				c == '#' ? Cell::Wall : Cell::Floor;
		}
	}
	DN_ASSERT(foundStart, "map has no 'P' start cell: " + path);

	// Fires thicken the air around them (braziers more than sconces).
	for (const auto& [bx, bz] : m_braziers) AddFireTurbidity(bx, bz, 0.55f);
	for (const auto& [tx, tz] : m_torches) AddFireTurbidity(tx, tz, 0.28f);

	log::Info("Loaded map {}: {}x{}, {} torches, {} braziers, {} monsters", path,
			  m_width, m_height, m_torches.size(), m_braziers.size(),
			  m_monsters.size());
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
