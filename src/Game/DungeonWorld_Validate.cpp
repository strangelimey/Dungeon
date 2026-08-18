// ============================================================================
// Game/DungeonWorld_Validate.cpp — the seam between the world and the
// playability checker (Game/Validate.h).
//
// All this does is GATHER: it turns "the project" into the snapshot the checker
// wants, and hands the catalog facts across. The analysis itself lives in
// Validate.cpp, which knows nothing about the world — that split is what lets
// the checker be reasoned about (and tested) without a running game.
//
// The active level is read from LIVE state, not from its file, so the check
// answers for what is on screen — including edits not yet saved, which is
// exactly when you want to be told a door has become unopenable.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Game/Catalog.h"

namespace dungeon::game {

bool DungeonWorld::CellFreeForStair(const std::string& stem, int x, int z) {
	const DungeonMap& m = stem == m_currentLevel ? m_map : EnsureMapStash(stem);
	return m.IsWalkable(x, z) && !m.StairAt(x, z) && !m.BrazierAt(x, z);
}

bool DungeonWorld::InstallLevelFromFiles(const std::string& stem,
										 const std::string& mapPath,
										 const std::string& entPath) {
	DungeonMap map(mapPath, FixtureTypesOf(m_project));
	DungeonEntities ents(entPath, map);

	if (stem != m_currentLevel) {
		// An inactive level is just its stash — the ordinary remote-edit path.
		EnsureMapStash(stem); // create the slots before taking references
		EnsureEntStash(stem);
		*m_levelMaps.find(stem)->second = std::move(map);
		*m_levelEnts.find(stem)->second = std::move(ents);
		return true;
	}

	// The ACTIVE level, replaced in place. This is RestoreEditorState's tail,
	// and for the same reasons: Party holds a reference to m_map so the object
	// must persist and only its data change; the surface rebake is deferred
	// because any cell may differ and the full-screen editor hides the scene
	// meanwhile (FlushGeometry pays for it once, on the way out).
	m_device.WaitIdle();
	const bool paletteChanged = m_map.WallPalette() != map.WallPalette() ||
								m_map.FloorPalette() != map.FloorPalette() ||
								m_map.CeilingPalette() != map.CeilingPalette();
	m_map = std::move(map);
	m_entities = std::move(ents);
	m_entsDirty = true;
	// THE PART RestoreEditorState DOES NOT NEED, and the reason this is not just
	// a call to it: a snapshot restores a map of the SAME dimensions, so nothing
	// sized to the grid ever had to change. A regenerate can hand back a level of
	// a different size, and everything parallel to the cells must be resized with
	// it — the fog mask first, which is indexed by MarkSeen a few lines below and
	// asserted out of range the first time this ran against a bigger level.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	m_walkableCache.reset(); // a grid built for the old map's bounds
	// Transient things positioned in the level that just ceased to exist.
	m_projectiles.Clear();
	m_pendingTransition.reset();
	m_pendingFall.reset();
	m_fallT = -1.0f;
	m_shadows.InvalidateCubes();
	// The dynamic layer comes from the new records alone: a regenerate discards
	// the old level, so there are no live diffs left worth carrying over — and
	// applying stale ones would resurrect objects from a dungeon that is gone.
	m_levelStates.erase(m_currentLevel);
	RespawnFromRecords();
	// The party is standing wherever the OLD level put it, which the new one may
	// have made solid rock. Put it on the new start cell — the one square the
	// generator guarantees is floor.
	m_party.SetGridPosition(m_map.StartX(), m_map.StartZ());
	MarkSeen(m_map.StartX(), m_map.StartZ());
	RebuildFiresAndDust();
	m_geometryDirty = true;
	if (paletteChanged) m_surfacesDirty = true;
	return true;
}

std::vector<validate::Issue> DungeonWorld::Validate() {
	// The catalog half of the rules. Both are id SETS rather than lookups so the
	// inner flood never touches a Catalog.
	validate::Rules rules;
	for (const CatalogEntry* e : m_project.AllItems())
		if (e && e->Get("category", "") == "key") rules.keyItems.insert(e->id);
	for (const CatalogEntry& e : m_project.stairs.Entries())
		if (CatalogBool(&e, "traverse", true)) rules.traversableStairs.insert(e.id);

	// The active level's decorations live as instances rather than records, and
	// the same is true of nothing else the checker reads — doors, items and
	// buttons are record-backed throughout. So the .ent side needs no sync here;
	// the map side does, because a live edit has not been written back.
	std::vector<validate::LevelView> views;
	views.reserve(m_project.levels.size());
	for (const std::string& stem : m_project.levels) {
		validate::LevelView v;
		v.stem = stem;
		if (stem == m_currentLevel) {
			v.map = &m_map;
			v.ents = &m_entities;
		} else {
			// Parsed on demand if this level has never been touched — the same
			// lazy stash SweepTypeRefs and the browse view use.
			v.map = &EnsureMapStash(stem);
			v.ents = &EnsureEntStash(stem);
		}
		views.push_back(std::move(v));
	}

	// Play begins on the manifest's FIRST level: that is what StartNewGame loads,
	// so it is where the flood has to start for the answer to mean anything.
	const std::string start =
		m_project.levels.empty() ? m_currentLevel : m_project.levels.front();
	return validate::Run(views, start, rules);
}

} // namespace dungeon::game
