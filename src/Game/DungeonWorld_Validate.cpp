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
