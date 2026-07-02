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
	int fileOrder = 0;     // record index in the file, skipped records included
	for (const std::string& line : ReadLevelLines(*bytes)) {
		Entity e = ParseEntityRecord(line, path);
		DN_ASSERT(e.kind != EntityKind::Decoration,
				  std::format("decorations are static — move \"{}\" to the .map file ({})",
							  line, path));
		// Ids number every record in the FILE, not the surviving vector, so a
		// skipped record never shifts a survivor's id — the level stash and the
		// save layer diff dynamic state by these ids across re-parses.
		e.id = fileOrder++;
		// Placement is checked with a skip + warning, not an assert: the editor
		// can repaint a record's cell solid (or open a button's mount wall) after
		// the .ent was written — live state is pruned at edit time (DungeonWorld::
		// PruneEntitiesForCell), but this file is only rewritten by an explicit
		// save, and the level stash re-parses it against the EDITED map on
		// re-entry. A stale record is a normal editor state, not an authoring bug.
		if (e.x < 0 || e.z < 0 || e.x >= map.Width() || e.z >= map.Height()) {
			log::Warn("Skipping out-of-bounds entity: \"{}\" in {}", line, path);
			continue;
		}
		if (!map.IsWalkable(e.x, e.z)) {
			log::Warn("Skipping entity in solid rock: \"{}\" in {}", line, path);
			continue;
		}
		if (e.kind == EntityKind::Button &&
			map.IsWalkable(e.x + DirDX(e.facing), e.z + DirDZ(e.facing))) {
			// Buttons mount on a wall: the faced neighbor must be solid.
			log::Warn("Skipping button facing open floor: \"{}\" in {}", line, path);
			continue;
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

Entity* DungeonEntities::MutableById(int id) {
	for (Entity& e : m_entities)
		if (e.id == id) return &e;
	return nullptr;
}

// erase_if keeps relative order, so the by-cell sort At() binary-searches holds.
size_t DungeonEntities::RemoveAt(int x, int z) {
	return std::erase_if(m_entities,
						 [&](const Entity& e) { return e.x == x && e.z == z; });
}

bool DungeonEntities::RemoveById(int id) {
	return std::erase_if(m_entities, [&](const Entity& e) { return e.id == id; }) > 0;
}

} // namespace dungeon::game
