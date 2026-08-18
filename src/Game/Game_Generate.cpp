// ============================================================================
// Game/Game_Generate.cpp — the seam between the level generator and the project.
//
// Game/Generate.h is pure and knows no catalogs, so everything catalog-shaped
// happens here: resolving the level's THEME TAGS into the id pools the generator
// picks from, and turning what it produces back into the ordinary .map/.ent text
// every other level is written as.
//
// The theme lens works exactly as it does in the palette (Catalog.h): an entry
// with no tags fits any theme, and a theme that matches nothing falls back to
// the whole pool rather than generating an empty dungeon — a knob that silently
// produces nothing teaches you to distrust the button, and "no monsters tagged
// undead yet" is a content gap, not a reason to refuse.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {

// Ids from a catalog whose tags match the theme, or ALL of them when nothing
// matches (see the header note).
std::vector<std::string> PoolFor(const Catalog& cat,
								 const std::vector<std::string>& theme,
								 const char* categoryFilter = nullptr) {
	std::vector<std::string> matched, all;
	for (const CatalogEntry& e : cat.Entries()) {
		if (CatalogBool(&e, "hidden", false)) continue;
		if (categoryFilter && e.Get("category", "") != categoryFilter) continue;
		all.push_back(e.id);
		if (CatalogMatchesTags(&e, theme)) matched.push_back(e.id);
	}
	return matched.empty() ? all : matched;
}

// The .ent record line for a generated entity. Deliberately the same shape the
// editor's own writer emits — a generated level has to be indistinguishable
// from a hand-built one, or half the tooling stops applying to it.
std::string RecordLine(const Entity& e) {
	const char* kind = e.kind == EntityKind::Monster  ? "monster"
					   : e.kind == EntityKind::Item   ? "item"
					   : e.kind == EntityKind::Door   ? "door"
													  : "button";
	// Direction order, matching the parser's tokens. A file-local table like the
	// editor writer's — there is no shared DirName to call.
	static const char* kDir[4] = {"north", "east", "south", "west"};
	std::string line = std::format("{} {} {} {} {}", kind, e.type, e.x, e.z,
								   kDir[static_cast<int>(e.facing)]);
	for (const auto& [k, v] : e.params) line += std::format(" {}={}", k, v);
	return line + "\n";
}

} // namespace

std::string Game::GenerateLevel(generate::Params params,
								const std::vector<std::string>& theme) {
	// --- content pools, by theme ---------------------------------------------
	params.monsterIds = PoolFor(m_project.monsters, theme);
	params.lootIds = PoolFor(m_project.items, theme);
	// Keys are the one pool that is NOT themed: a lock needs a key that exists,
	// and which key it is matters far less than that the pair is coherent. An
	// empty pool simply means no locks get authored (Generate.h clamps to it).
	params.keyIds.clear();
	for (const CatalogEntry* e : m_project.AllItems())
		if (e && e->Get("category", "") == "key") params.keyIds.push_back(e->id);
	// Loot should not hand out the keys as treasure — that would let a key turn
	// up behind its own door, which is precisely the fault the construction
	// order exists to prevent.
	std::erase_if(params.lootIds, [&](const std::string& id) {
		return std::find(params.keyIds.begin(), params.keyIds.end(), id) !=
			   params.keyIds.end();
	});

	const generate::Level lv = generate::Run(params);

	// --- next free stem ------------------------------------------------------
	int maxN = 0;
	for (const std::string& s : m_project.levels)
		if (s.starts_with("level"))
			if (const int n = std::atoi(s.c_str() + 5); n > maxN) maxN = n;
	const std::string stem = "level" + std::to_string(maxN + 1);
	if (std::find(m_project.levels.begin(), m_project.levels.end(), stem) !=
		m_project.levels.end()) {
		log::Warn("generate: stem {} already exists", stem);
		return {};
	}

	// --- the static layer ----------------------------------------------------
	auto join = [](const std::vector<std::string>& ids) {
		std::string out;
		for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
		return out;
	};
	const DungeonMap& donor = m_world.Map(); // palettes come from the active level
	std::string map = std::format("; {} - generated (seed {}).\n", stem, params.seed);
	map += "palette wall " + join(donor.WallPalette()) + "\n";
	map += "palette floor " + join(donor.FloorPalette()) + "\n";
	map += "palette ceiling " + join(donor.CeilingPalette()) + "\n";
	if (!theme.empty()) map += "theme " + join(theme) + "\n";
	map += ";\n";
	for (int z = 0; z < lv.height; ++z) {
		for (int x = 0; x < lv.width; ++x)
			map += !lv.At(x, z)                            ? '#'
				   : (x == lv.startX && z == lv.startZ)    ? 'P'
														   : '.';
		map += '\n';
	}

	std::string ent = std::format("; {} - generated dynamic layer.\n", stem);
	for (const Entity& e : lv.entities) ent += RecordLine(e);

	const std::string mapOut = serialize::NormalizeEol(map);
	const std::string entOut = serialize::NormalizeEol(ent);
	if (!assets::WriteBinaryFile(m_project.LevelMapPath(stem), mapOut.data(),
								 mapOut.size()) ||
		!assets::WriteBinaryFile(m_project.LevelEntPath(stem), entOut.data(),
								 entOut.size())) {
		log::Warn("generate: failed to write {} files", stem);
		return {};
	}
	m_project.levels.push_back(stem);
	m_project.Save();
	return stem;
}

} // namespace dungeon::game
