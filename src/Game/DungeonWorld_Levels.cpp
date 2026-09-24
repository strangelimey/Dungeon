// ============================================================================
// Game/DungeonWorld_Levels.cpp — split out of DungeonWorld_Editing.cpp to keep
// files small (see DungeonWorld.h). Holds the operations on a level's FILES as
// a whole rather than on its contents: saving every edited level, renaming
// one, and deleting a dungeon's levels (W10).
//
// What they share is the obligation that makes them worth a file: a level lives
// in THREE places — its two files, the per-level stashes (m_levelMaps /
// m_levelEnts / m_levelStates) and, for the far side of a stair, every OTHER
// level's records. Each operation here has to reach all three, or the next
// `savemap` writes back what was renamed or deleted.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Log.h"

#include <algorithm>
#include <filesystem>

namespace dungeon::game {

std::vector<std::string> DungeonWorld::SaveAllLevels() {
	std::vector<std::string> saved;
	if (SaveLevel()) saved.push_back(m_currentLevel);
	for (const auto& [stem, map] : m_levelMaps)
		if (WriteStashedLevel(stem)) saved.push_back(stem);
	return saved;
}

bool DungeonWorld::RenameLevel(const std::string& oldStem,
							   const std::string& newStem) {
	// Disk first (the .ent failure path can roll the .map back, keeping the
	// pair consistent) — a stash-backed level renames its files too, so the
	// next savemap writes to the new paths and no stale pair lingers.
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::rename(m_project.LevelMapPath(oldStem), m_project.LevelMapPath(newStem),
			   ec);
	if (ec) {
		log::Warn("rename level: map move failed: {}", ec.message());
		return false;
	}
	fs::rename(m_project.LevelEntPath(oldStem), m_project.LevelEntPath(newStem),
			   ec);
	if (ec) {
		std::error_code undo;
		fs::rename(m_project.LevelMapPath(newStem),
				   m_project.LevelMapPath(oldStem), undo);
		log::Warn("rename level: ent move failed: {}", ec.message());
		return false;
	}

	// In-memory keys follow the stem: the three per-level stashes...
	auto rekey = [&](auto& stash) {
		auto it = stash.find(oldStem);
		if (it == stash.end()) return;
		auto node = std::move(it->second);
		stash.erase(oldStem);
		stash.insert_or_assign(newStem, std::move(node));
	};
	rekey(m_levelMaps);
	rekey(m_levelEnts);
	if (auto it = m_levelStates.find(oldStem); it != m_levelStates.end()) {
		SaveData::LevelState state = std::move(it->second);
		m_levelStates.erase(oldStem);
		state.stem = newStem; // the block writes its own stem line
		m_levelStates.insert_or_assign(newStem, std::move(state));
	}
	// ...and the active stem.
	if (m_currentLevel == oldStem) m_currentLevel = newStem;

	// Repoint every stair dest= that names the old stem: the active map is
	// fixed live, every other level via its stash — EnsureMapStash lazily
	// parses disk-only levels. (The caller updates Project::levels after this
	// returns, so the walk still sees the OLD stem in the list — map it to the
	// new one.) EXITS ARE SKIPPED: their dest is a world location, and one
	// spelled like the old stem is not the thing being renamed (W11).
	std::vector<std::string> exits;
	for (const CatalogEntry& e : m_project.stairs.Entries())
		if (CatalogBool(&e, "exit", false)) exits.push_back(e.id);
	// AND WRITTEN NOW, not on the next savemap (W11). The files above have
	// already moved and the owner saves the manifest straight after, so a
	// session ended before a savemap used to leave every other level's stairs
	// on disk naming a level that no longer existed — a stair that aborts the
	// game when taken. Only the levels a stair actually changed in are
	// written.
	std::vector<std::string> touched;
	if (m_map.RenameStairDest(oldStem, newStem, exits) > 0) {
		if (SaveLevel()) touched.push_back(m_currentLevel);
	}
	for (const std::string& stem : m_project.levels) {
		const std::string& actual = stem == oldStem ? newStem : stem;
		if (actual == m_currentLevel) continue;
		if (EnsureMapStash(actual).RenameStairDest(oldStem, newStem, exits) > 0 &&
			WriteStashedLevel(actual))
			touched.push_back(actual);
	}
	for (const std::string& stem : touched)
		log::Info("rename level: repointed stairs written in {}", stem);

	// Undo snapshots hold whole stash sets keyed by the old stem (and the old
	// file paths' contents); restoring one across a rename would resurrect the
	// dead name. Renames are rare — drop the history like a level transition.
	ClearUndoHistory();

	// NOTE: existing save FILES still reference the old stem (save current= /
	// level blocks) — loading one after a rename will die on the missing
	// level. Editor-side renames assume dev-cycle saves; re-save after.
	log::Info("Renamed level {} -> {}", oldStem, newStem);
	return true;
}

std::vector<DungeonWorld::StairInto>
DungeonWorld::StairsInto(const std::vector<std::string>& dying) {
	const auto isDying = [&](const std::string& stem) {
		return std::find(dying.begin(), dying.end(), stem) != dying.end();
	};
	std::vector<StairInto> found;
	const auto scan = [&](const std::string& from, const DungeonMap& map) {
		for (const StairLink& s : map.Stairs()) {
			// An EXIT names a world location, not a level (DungeonWorld.cpp's
			// transition reads it that way), so it is never a way into one.
			if (CatalogBool(m_project.stairs.Find(s.type), "exit", false)) continue;
			if (isDying(s.destLevel)) found.push_back({from, s.x, s.z, s.destLevel});
		}
	};
	for (const std::string& stem : m_project.levels) {
		// A stair INSIDE the dungeon being deleted goes with it — that is the
		// dungeon's own plumbing, not a reference into it.
		if (isDying(stem)) continue;
		if (stem == m_currentLevel) scan(stem, m_map);
		else scan(stem, EnsureMapStash(stem));
	}
	return found;
}

bool DungeonWorld::DeleteLevel(const std::string& stem) {
	if (stem == m_currentLevel) {
		log::Warn("delete level '{}': it is the active level - refused", stem);
		return false;
	}
	// MEMORY FIRST. A stash left behind is a level the next savemap writes
	// straight back to disk (SaveAllLevels walks m_levelMaps), and a dynamic
	// state left behind would ride the next save file under a dead stem.
	m_levelMaps.erase(stem);
	m_levelEnts.erase(stem);
	m_levelStates.erase(stem);

	namespace fs = std::filesystem;
	bool ok = true;
	for (const std::string& path :
		 {m_project.LevelMapPath(stem), m_project.LevelEntPath(stem)}) {
		std::error_code ec;
		// remove() is false for a file that was never there, which is not a
		// failure: a level whose .ent was never written has nothing to lose.
		fs::remove(path, ec);
		if (ec) {
			log::Warn("delete level '{}': {} ({})", stem, path, ec.message());
			ok = false;
		}
	}
	if (ok) log::Info("Deleted level {}", stem);
	return ok;
}

} // namespace dungeon::game
