// ============================================================================
// Game/DungeonEntities.h — the dynamic layer of a level: everything that can
// change during play.
//
// Loaded from the project's levels/<stem>.ent (assets/projects/<name>/levels
// — see Project::EntPath), one record per line (format in Entity.h): monsters,
// items, buttons, doors. This file is the level's INITIAL dynamic state; the
// save layer stores the live counterparts as a per-level DIFF against it
// (DungeonWorld::SnapshotActive / SaveData::LevelState), while the static
// structure (DungeonMap, the .map file) never needs saving. Static decorations
// live in the .map file, not here.
//
// Records are validated against the map at load (in bounds, monsters and
// items on walkable cells, buttons mounted on a solid wall) and sorted by
// cell so gameplay can ask "what is in the cell ahead?" via At(). A record
// that contradicts the map is SKIPPED with a warning, not a fatal assert:
// the editor can repaint a cell solid after the .ent was authored (live
// state is pruned at edit time, but this file is only rewritten by an
// explicit save, and the level stash re-parses it against the EDITED map on
// re-entry — see DungeonWorld::PruneEntitiesForCell / BeginLevelLoad).
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
	// Loads and validates a .ent file. Syntax failures are fatal with a clear
	// message; records contradicting the map are skipped with a warning (see
	// the file banner).
	DungeonEntities(const std::string& path, const DungeonMap& map);

	const std::vector<Entity>& All() const { return m_entities; }
	// Appends an editor-authored record (remote-level placement), assigning the
	// next stable id above every existing one (ids are file-record order and
	// removals never renumber, so max+1 is always fresh) and inserting at the
	// by-cell sort position At() binary-searches. Returns the new id.
	int Add(Entity record);
	// Mutable lookup by stable spawn id, for the editor's instance inspector
	// (e.g. editing a placed item's facing). nullptr if no such entity.
	Entity* MutableById(int id);
	// Removes every record standing on the cell (an editor paint just buried
	// it). Survivor ids are untouched. Returns the number removed.
	size_t RemoveAt(int x, int z);
	// Removes one record by stable id (a button whose mount wall was opened
	// with no replacement wall). False if no such record.
	bool RemoveById(int id);

	// Every entity in one cell (possibly several — an item on a pressure
	// plate, a monster guarding both).
	std::span<const Entity> At(int x, int z) const;

	// Editor type rename/delete: counts the records of `kind` whose type is
	// `id` and, when `newId` is given, retypes them. The dynamic layer is one of
	// the places a catalog id is referenced, so a rename has to reach it — else
	// the level would load records naming a type that no longer exists.
	int SweepTypeRefs(EntityKind kind, std::string_view id,
					  const std::string* newId);

private:
	std::vector<Entity> m_entities; // sorted by (z * map width + x)
	int m_width = 0;
};

} // namespace dungeon::game
