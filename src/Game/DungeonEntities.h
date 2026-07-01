// ============================================================================
// Game/DungeonEntities.h — the dynamic layer of a level: everything that can
// change during play.
//
// Loaded from assets/maps/<level>.ent, one record per line (format in
// Entity.h): monsters, items, buttons. This file is the level's INITIAL
// dynamic state — a future save system serializes the live counterparts of
// these records, while the static structure (DungeonMap, the .map file)
// never needs saving. Static decorations live in the .map file, not here.
//
// Records are validated against the map at load (in bounds, monsters and
// items on walkable cells, buttons mounted on a solid wall) and sorted by
// cell so gameplay can ask "what is in the cell ahead?" via At().
// ============================================================================
#pragma once

#include "Game/DungeonMap.h"
#include "Game/Entity.h"

#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

class DungeonEntities {
public:
	// Loads and validates a .ent file; failures are fatal with a clear message.
	DungeonEntities(const std::string& path, const DungeonMap& map);

	const std::vector<Entity>& All() const { return m_entities; }
	// Mutable lookup by stable spawn id, for the editor's instance inspector
	// (e.g. editing a placed item's facing). nullptr if no such entity.
	Entity* MutableById(int id);

	// Every entity in one cell (possibly several — an item on a pressure
	// plate, a monster guarding both).
	std::span<const Entity> At(int x, int z) const;

private:
	std::vector<Entity> m_entities; // sorted by (z * map width + x)
	int m_width = 0;
};

} // namespace dungeon::game
