// ============================================================================
// Game/Area.cpp - rooms and corridors as cell sets. See Area.h.
// ============================================================================
#include "Game/Area.h"

namespace dungeon::game::area {

namespace {
constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
}

bool IsOpen(const DungeonMap& map, int x, int z) {
	if (!map.IsWalkable(x, z)) return false;
	// The four 2x2 blocks that contain (x,z): its corner at (x+dx, z+dz).
	for (int dz = -1; dz <= 0; ++dz)
		for (int dx = -1; dx <= 0; ++dx)
			if (map.IsWalkable(x + dx, z + dz) && map.IsWalkable(x + dx + 1, z + dz) &&
				map.IsWalkable(x + dx, z + dz + 1) && map.IsWalkable(x + dx + 1, z + dz + 1))
				return true;
	return false;
}

std::vector<CellXZ> Region(const DungeonMap& map, int x, int z) {
	std::vector<CellXZ> region;
	const int w = map.Width(), h = map.Height();
	if (x < 0 || z < 0 || x >= w || z >= h || !map.IsWalkable(x, z)) return region;
	const bool open = IsOpen(map, x, z);
	std::vector<unsigned char> seen(static_cast<size_t>(w) * h, 0);
	std::vector<CellXZ> stack{{x, z}};
	seen[static_cast<size_t>(z) * w + x] = 1;
	while (!stack.empty()) {
		const CellXZ c = stack.back();
		stack.pop_back();
		region.push_back(c);
		for (const auto& s : kSteps) {
			const int nx = c.first + s[0], nz = c.second + s[1];
			if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
			unsigned char& mark = seen[static_cast<size_t>(nz) * w + nx];
			if (mark || !map.IsWalkable(nx, nz) || IsOpen(map, nx, nz) != open) continue;
			mark = 1;
			stack.push_back({nx, nz});
		}
	}
	return region;
}

std::vector<CellXZ> Walls(const DungeonMap& map, std::span<const CellXZ> region) {
	std::vector<CellXZ> walls;
	const int w = map.Width(), h = map.Height();
	std::vector<unsigned char> seen(static_cast<size_t>(w) * h, 0);
	for (const CellXZ& c : region)
		for (const auto& s : kSteps) {
			const int nx = c.first + s[0], nz = c.second + s[1];
			if (nx < 0 || nz < 0 || nx >= w || nz >= h || map.IsWalkable(nx, nz)) continue;
			unsigned char& mark = seen[static_cast<size_t>(nz) * w + nx];
			if (mark) continue;
			mark = 1;
			walls.push_back({nx, nz});
		}
	return walls;
}

} // namespace dungeon::game::area
