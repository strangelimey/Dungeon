#include "Game/DungeonEntities.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

DungeonEntities::DungeonEntities(const std::string& path, const DungeonMap& map)
	: m_width(map.Width()) {
	auto bytes = assets::ReadBinaryFile(path);
	DN_ASSERT(bytes.has_value(), bytes.error());

	size_t counts[4] = {}; // indexed by EntityKind, for the load log
	for (const std::string& line : ReadLevelLines(*bytes)) {
		Entity e = ParseEntityRecord(line, path);
		DN_ASSERT(e.kind != EntityKind::Decoration,
				  std::format("decorations are static — move \"{}\" to the .map file ({})",
							  line, path));
		DN_ASSERT(e.x >= 0 && e.z >= 0 && e.x < map.Width() && e.z < map.Height(),
				  std::format("entity out of bounds: \"{}\" in {}", line, path));
		DN_ASSERT(map.IsWalkable(e.x, e.z),
				  std::format("entity in solid rock: \"{}\" in {}", line, path));
		if (e.kind == EntityKind::Button) {
			// Buttons mount on a wall: the faced neighbor must be solid.
			DN_ASSERT(!map.IsWalkable(e.x + DirDX(e.facing), e.z + DirDZ(e.facing)),
					  std::format("button must face a solid wall: \"{}\" in {}", line, path));
		}
		++counts[static_cast<size_t>(e.kind)];
		m_entities.push_back(std::move(e));
	}

	// Sort by cell so At() can binary-search; stable keeps file order per cell.
	std::ranges::stable_sort(m_entities, {}, [this](const Entity& e) {
		return e.z * m_width + e.x;
	});

	log::Info("Loaded entities {}: {} monsters, {} items, {} buttons", path,
			  counts[static_cast<size_t>(EntityKind::Monster)],
			  counts[static_cast<size_t>(EntityKind::Item)],
			  counts[static_cast<size_t>(EntityKind::Button)]);
}

std::span<const Entity> DungeonEntities::At(int x, int z) const {
	const auto [first, last] =
		std::ranges::equal_range(m_entities, z * m_width + x, {}, [this](const Entity& e) {
			return e.z * m_width + e.x;
		});
	return {first, last};
}

} // namespace dungeon::game
