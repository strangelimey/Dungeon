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

std::pair<int, int> DungeonWorld::FarthestStairCell(const std::string& stem) {
	const DungeonMap& m = stem == m_currentLevel ? m_map : EnsureMapStash(stem);
	const int w = m.Width(), h = m.Height();
	// Breadth-first over walkable squares (4-connected, the grid's only kind of
	// step), keeping the LAST stair-worthy square reached: BFS visits in
	// distance order, so that is the farthest one. From EVERY way in at once —
	// the start and each stair already there — because on a lower floor the
	// party arrives by the stair, not at `P`: measured from `P` alone, crypt2's
	// way down landed one square from its own way up.
	std::vector<u8> seen(static_cast<size_t>(w) * h, 0);
	std::vector<std::pair<int, int>> queue;
	auto source = [&](int x, int z) {
		if (x < 0 || z < 0 || x >= w || z >= h || !m.IsWalkable(x, z)) return;
		u8& s = seen[static_cast<size_t>(z) * w + x];
		if (!s) s = 1, queue.push_back({x, z});
	};
	source(m.StartX(), m.StartZ());
	for (const StairLink& st : m.Stairs()) source(st.x, st.z);
	std::pair<int, int> best{-1, -1};
	for (size_t head = 0; head < queue.size(); ++head) {
		const auto [x, z] = queue[head];
		if (!m.StairAt(x, z) && !m.BrazierAt(x, z)) best = {x, z};
		constexpr int dx[4] = {0, 1, 0, -1};
		constexpr int dz[4] = {-1, 0, 1, 0};
		for (int i = 0; i < 4; ++i) {
			const int nx = x + dx[i], nz = z + dz[i];
			if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
			u8& s = seen[static_cast<size_t>(nz) * w + nx];
			if (s || !m.IsWalkable(nx, nz)) continue;
			s = 1;
			queue.push_back({nx, nz});
		}
	}
	return best;
}

bool DungeonWorld::InstallLevelFromFiles(const std::string& stem,
										 const std::string& mapPath,
										 const std::string& entPath) {
	DungeonMap map(mapPath, FixtureTypesOf(m_project));
	DungeonEntities ents(entPath, map);
	return InstallLevel(stem, std::move(map), std::move(ents));
}

bool DungeonWorld::InstallLevelFromText(const std::string& stem,
										std::string_view mapText,
										std::string_view entText) {
	// A RANDOM ENCOUNTER, which exists only in memory (docs/world-map.md). It
	// takes the same install as a level read from disk, deliberately: an
	// encounter is ORDINARY CONTENT and the moment it needed its own path
	// through the world it would start behaving differently from the dungeons
	// it is pretending to be.
	DungeonMap map = DungeonMap::FromText(mapText, FixtureTypesOf(m_project), stem);
	DungeonEntities ents = DungeonEntities::FromText(entText, map, stem);
	// ALWAYS THE ACTIVE LEVEL, never a stash. The stash branch below loads the
	// level FROM FILE to create its slot, which for a level that has no file is
	// an abort — and it aborted, on the first run. An encounter has nowhere to
	// be stashed TO in any case: it is thrown away, not returned to.
	//
	// Naming it current BEFORE the install is what selects that branch, and it
	// is also true: the moment the map is swapped, this is where the party is.
	// Nothing is stashed on the way out, so the level being left simply ends,
	// which is what a throwaway space deserves.
	//
	// EXCEPT A LEVEL THE PARTY WALKED OUT OF. An ambush on the road replaces the
	// dungeon still loaded under the world map, and that one is returned to —
	// so it was PARKED on the way out (ParkActive), and its stash stands.
	m_currentLevel = stem;
	return InstallLevel(stem, std::move(map), std::move(ents));
}

bool DungeonWorld::InstallLevel(const std::string& stem, DungeonMap&& map,
								DungeonEntities&& ents) {
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
	m_parked = false; // whatever was parked here has been replaced
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

std::vector<validate::Issue> DungeonWorld::Validate(const validate::WorldView& world) {
	// The catalog half of the rules. Both are id SETS rather than lookups so the
	// inner flood never touches a Catalog.
	validate::Rules rules;
	for (const CatalogEntry* e : m_project.AllItems())
		if (e && e->Get("category", "") == "key") rules.keyItems.insert(e->id);
	for (const CatalogEntry& e : m_project.stairs.Entries()) {
		if (CatalogBool(&e, "traverse", true)) rules.traversableStairs.insert(e.id);
		if (CatalogBool(&e, "exit", false)) rules.exitStairs.insert(e.id);
	}

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
	return validate::Run(views, start, rules, world);
}

} // namespace dungeon::game
