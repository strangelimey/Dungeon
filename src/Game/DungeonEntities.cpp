#include "Game/DungeonEntities.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <cstdlib> // atoi - the squares in leashfrom= / patrol=
#include <format>

namespace dungeon::game {

DungeonEntities::DungeonEntities(const std::string& path, const DungeonMap& map)
	: m_width(map.Width()) {
	auto bytes = assets::ReadBinaryFile(path);
	DN_ASSERT(bytes.has_value(), bytes.error());
	Parse(*bytes, map, path);
}

DungeonEntities DungeonEntities::FromText(std::string_view text,
										  const DungeonMap& map,
										  const std::string& where) {
	DungeonEntities e;
	e.m_width = map.Width();
	e.Parse(std::vector<u8>(text.begin(), text.end()), map, where);
	return e;
}

void DungeonEntities::Parse(const std::vector<u8>& bytesIn, const DungeonMap& map,
							const std::string& path) {
	const std::vector<u8>* bytes = &bytesIn;

	size_t counts[5] = {}; // indexed by EntityKind, for the load log
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
	// Past every id the file numbered, skipped records' included: a record the
	// editor adds can never take one a held state still names.
	m_nextId = fileOrder;

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

void DungeonEntities::Reframe(int dx, int dz, int newWidth) {
	// "x,z" -> shifted, or the text unchanged when it is not a pair of numbers
	// (the loader decides what a malformed value means; this only moves squares).
	auto shiftCell = [&](std::string_view cell) -> std::string {
		const size_t comma = cell.find(',');
		if (comma == std::string_view::npos) return std::string(cell);
		const int x = std::atoi(std::string(cell.substr(0, comma)).c_str());
		const int z = std::atoi(std::string(cell.substr(comma + 1)).c_str());
		return std::format("{},{}", x + dx, z + dz);
	};
	for (Entity& e : m_entities) {
		e.x += dx;
		e.z += dz;
		// The only params that name a square (DungeonWorld_Load's monster parse):
		// leashfrom=x,z and patrol=x,z;x,z;...
		for (auto& [key, value] : e.params) {
			if (key == "leashfrom") {
				value = shiftCell(value);
			} else if (key == "patrol") {
				std::string out;
				size_t start = 0;
				while (start <= value.size()) {
					const size_t end = std::min(value.find(';', start), value.size());
					if (!out.empty()) out += ';';
					out += shiftCell(std::string_view(value).substr(start, end - start));
					start = end + 1;
				}
				value = std::move(out);
			}
		}
	}
	m_width = newWidth;
	std::ranges::stable_sort(m_entities, {}, [this](const Entity& e) {
		return e.z * m_width + e.x;
	});
}

int DungeonEntities::Add(Entity record) {
	// Monotonic (C327). It was max(id)+1, which a removal of the highest id
	// turned back - and EraseRemote leaves that id's diff in the level's held
	// state, which ApplyActiveSnapshot lays on whatever record now carries it.
	record.id = m_nextId++;
	const int key = record.z * m_width + record.x;
	const auto pos = std::ranges::upper_bound(
		m_entities, key, {}, [this](const Entity& e) { return e.z * m_width + e.x; });
	return m_entities.insert(pos, std::move(record))->id;
}

bool DungeonEntities::Replace(Entity record) {
	const auto it =
		std::ranges::find_if(m_entities, [&](const Entity& e) { return e.id == record.id; });
	if (it == m_entities.end()) return false;
	if (it->x == record.x && it->z == record.z) {
		*it = std::move(record);
		return true;
	}
	// A new cell: out and back in at its sort position (the id goes with it).
	m_entities.erase(it);
	const int key = record.z * m_width + record.x;
	const auto pos = std::ranges::upper_bound(
		m_entities, key, {}, [this](const Entity& e) { return e.z * m_width + e.x; });
	m_entities.insert(pos, std::move(record));
	return true;
}

Entity* DungeonEntities::MutableById(int id) {
	for (Entity& e : m_entities)
		if (e.id == id) return &e;
	return nullptr;
}

const Entity* DungeonEntities::ById(int id) const {
	for (const Entity& e : m_entities)
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

int DungeonEntities::SweepTypeRefs(EntityKind kind, std::string_view id,
								   const std::string* newId) {
	int hits = 0;
	for (Entity& e : m_entities) {
		if (e.kind != kind || e.type != id) continue;
		++hits;
		if (newId) e.type = *newId;
	}
	return hits;
}

int DungeonEntities::SweepFlagRefs(std::string_view id, const std::string* newId) {
	int hits = 0;
	for (Entity& e : m_entities) {
		if (e.kind != EntityKind::Door && e.kind != EntityKind::Button) continue;
		for (auto& [k, v] : e.params) {
			if (v != id || (k != "flag" && k != "sets" && k != "clears" && k != "toggles"))
				continue;
			++hits;
			if (newId) v = *newId;
		}
	}
	return hits;
}

} // namespace dungeon::game
