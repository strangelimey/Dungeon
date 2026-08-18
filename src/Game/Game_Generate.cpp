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
//
// TWO ENTRY POINTS, one builder. GenerateLevel writes a NEW level (files,
// manifest, and the stair that joins it to the project); RegenerateViewedLevel
// replaces the level you are looking at, in place, as one undo step. They share
// BuildLevelText so the same knobs cannot produce two different dungeons.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <system_error>

namespace dungeon::game {

namespace {

// Ids from a catalog whose tags match the theme, or ALL of them when nothing
// matches (see the header note).
std::vector<std::string> PoolFor(const Catalog& cat,
								 const std::vector<std::string>& theme) {
	std::vector<std::string> matched, all;
	for (const CatalogEntry& e : cat.Entries()) {
		if (CatalogBool(&e, "hidden", false)) continue;
		all.push_back(e.id);
		if (CatalogMatchesTags(&e, theme)) matched.push_back(e.id);
	}
	return matched.empty() ? all : matched;
}

// The .ent record line for a generated entity. Deliberately the same shape the
// editor's own writer emits — a generated level has to be indistinguishable
// from a hand-built one, or half the tooling stops applying to it.
std::string RecordLine(const Entity& e) {
	const char* kind = e.kind == EntityKind::Monster ? "monster"
					   : e.kind == EntityKind::Item  ? "item"
					   : e.kind == EntityKind::Door  ? "door"
													 : "button";
	// Direction order, matching the parser's tokens. A file-local table like the
	// editor writer's — there is no shared DirName to call.
	static const char* kDir[4] = {"north", "east", "south", "west"};
	std::string line = std::format("{} {} {} {} {}", kind, e.type, e.x, e.z,
								   kDir[static_cast<int>(e.facing)]);
	for (const auto& [k, v] : e.params) line += std::format(" {}={}", k, v);
	return line + "\n";
}

// The generated level as the two files' TEXT. Shared by both entry points so
// they cannot drift into producing different dungeons from the same knobs.
void BuildLevelText(const std::string& stem, const generate::Level& lv,
					const generate::Params& params, const DungeonMap& donor,
					const std::vector<std::string>& theme, std::string& map,
					std::string& ent) {
	auto join = [](const std::vector<std::string>& ids) {
		std::string out;
		for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
		return out;
	};
	map = std::format("; {} - generated (seed {}).\n", stem, params.seed);
	map += "palette wall " + join(donor.WallPalette()) + "\n";
	map += "palette floor " + join(donor.FloorPalette()) + "\n";
	map += "palette ceiling " + join(donor.CeilingPalette()) + "\n";
	if (!theme.empty()) map += "theme " + join(theme) + "\n";
	map += ";\n";
	for (int z = 0; z < lv.height; ++z) {
		for (int x = 0; x < lv.width; ++x)
			map += !lv.At(x, z)                         ? '#'
				   : (x == lv.startX && z == lv.startZ) ? 'P'
														: '.';
		map += '\n';
	}
	ent = std::format("; {} - generated dynamic layer.\n", stem);
	for (const Entity& e : lv.entities) ent += RecordLine(e);
}

} // namespace

void Game::FillPools(generate::Params& params,
					 const std::vector<std::string>& theme) {
	params.monsterIds = PoolFor(m_project.monsters, theme);
	params.lootIds = PoolFor(m_project.items, theme);
	// Keys are the one pool that is NOT themed: a lock needs a key that exists,
	// and which key it is matters far less than that the pair is coherent. An
	// empty pool simply means no locks get authored (Generate.h clamps to it).
	params.keyIds.clear();
	for (const CatalogEntry* e : m_project.AllItems())
		if (e && e->Get("category", "") == "key") params.keyIds.push_back(e->id);
	// Loot must not hand out the keys as treasure — that would let a key turn up
	// behind its own door, precisely the fault the construction order prevents.
	std::erase_if(params.lootIds, [&](const std::string& id) {
		return std::find(params.keyIds.begin(), params.keyIds.end(), id) !=
			   params.keyIds.end();
	});
}

std::string Game::GenerateLevel(generate::Params params,
								const std::vector<std::string>& theme) {
	FillPools(params, theme);
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

	std::string map, ent;
	BuildLevelText(stem, lv, params, m_world.Map(), theme, map, ent);
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

	// --- join it to the project ----------------------------------------------
	// Without a stair the new level is unreachable, and the checker rightly says
	// so. The pair is authored through AddStairAt so it matches what the EDITOR
	// would write — each side arriving on its counterpart. Phase 3 found five
	// hand-made stairs that do not match that, and a generator is the last thing
	// that should manufacture more of them.
	//
	// AddStairAt puts both halves on the SAME cell, so the link needs a square
	// that suits BOTH levels. The generated start is tried first (arriving there
	// is what a player expects), then any other floor cell.
	const std::vector<std::string>& lvls = m_project.levels;
	if (lvls.size() >= 2) {
		const std::string prev = lvls[lvls.size() - 2];
		std::string downType;
		for (const CatalogEntry& e : m_project.stairs.Entries())
			if (!CatalogBool(&e, "up", false) && CatalogBool(&e, "traverse", true)) {
				downType = e.id;
				break;
			}
		std::vector<std::pair<int, int>> tries{{lv.startX, lv.startZ}};
		for (int z = 1; z < lv.height - 1; ++z)
			for (int x = 1; x < lv.width - 1; ++x)
				if (lv.At(x, z)) tries.push_back({x, z});
		bool linked = false;
		if (!downType.empty())
			for (const auto& [x, z] : tries)
				if (m_world.CellFreeForStair(prev, x, z) &&
					m_world.CellFreeForStair(stem, x, z)) {
					linked = m_world.AddStairAt(prev, downType, x, z);
					if (linked) break;
				}
		if (!linked)
			log::Warn("generate: {} has no stair from {} - no cell suits both",
					  stem, prev);
	}
	return stem;
}

bool Game::BuildAndInstall(const std::string& stem, const generate::Params& params,
						   const std::vector<std::string>& theme) {
	generate::Params p = params;
	FillPools(p, theme);
	const generate::Level lv = generate::Run(p);

	std::string map, ent;
	BuildLevelText(stem, lv, p, m_world.Map(), theme, map, ent);

	// Parsed through a TEMP file rather than the level's own, so the real files
	// stay untouched until `savemap` — which is how every other editor edit
	// behaves, and what keeps an undo COMPLETE instead of leaving the generated
	// version on disk after the reroll has been taken back. (DungeonMap only
	// constructs from a path; giving it a text constructor for this one caller
	// would be the larger change.)
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec) / "dungeon-gen";
	fs::create_directories(dir, ec);
	const std::string mapPath = (dir / (stem + ".map")).string();
	const std::string entPath = (dir / (stem + ".ent")).string();
	const std::string mapOut = serialize::NormalizeEol(map);
	const std::string entOut = serialize::NormalizeEol(ent);
	if (!assets::WriteBinaryFile(mapPath, mapOut.data(), mapOut.size()) ||
		!assets::WriteBinaryFile(entPath, entOut.data(), entOut.size())) {
		log::Warn("generate: could not stage {} for parsing", stem);
		return false;
	}
	const bool ok = m_world.InstallLevelFromFiles(stem, mapPath, entPath);
	fs::remove(mapPath, ec);
	fs::remove(entPath, ec);
	return ok;
}

bool Game::RegenerateViewedLevel(generate::Params params) {
	const std::string stem = m_mapView.ViewedLevel();
	if (stem.empty()) return false;
	// ONE undo step, and no level transition — see the declaration in Game.h.
	m_world.BeginUndoStep();
	const bool ok = BuildAndInstall(stem, params, m_mapView.ViewedMap().Theme());
	m_world.CommitUndoStep(ok);
	return ok;
}

} // namespace dungeon::game
